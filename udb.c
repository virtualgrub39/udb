#include <gio/gio.h>
#include <glib.h>

#include <signal.h>

#include "config.h"

#define UDB_UNUSED(var) (void)var;
#define UDB_TODO(msg) g_printerr("%s:%u TODO: %s\n", __FILE__, __LINE__, msg)

char* socket_path = UDB_SOCKET_PATH_DEFAULT;
char* db_file_path = NULL;
static GOptionEntry cmd_entries[] = {
    {
        .long_name        = "socket-path", .short_name = 'p',
        .description      = "Path to file where the unix socket will be created",
        .arg              = G_OPTION_ARG_STRING,
        .flags            = G_OPTION_FLAG_NONE,
        .arg_data         = &socket_path,
        .arg_description  = "PATH",
    },
    {
        .long_name        = "db-file", .short_name = 'f',
        .description      = "Path to file where database state will be saved",
        .arg              = G_OPTION_ARG_FILENAME,
        .flags            = G_OPTION_FLAG_NONE,
        .arg_data         = &db_file_path,
        .arg_description  = "FILE",
    },
    G_OPTION_ENTRY_NULL,
};

static GMainLoop* loop;
static volatile sig_atomic_t got_sigint = 0;
static GHashTable* db_mem = NULL;
static GMutex db_mutex;

static void
sigint_handler(int signum)
{
    UDB_UNUSED(signum);
    got_sigint = 1;
}

void
db_init(void)
{
    g_mutex_init(&db_mutex);
    db_mem = g_hash_table_new_full(
        g_str_hash,
        g_str_equal,
        g_free,
        g_free
    );
}

void
db_save_to_file(GError **error)
{
    g_return_if_fail(db_file_path != NULL);

    GKeyFile *keyfile = g_key_file_new();
    if (!keyfile) {
        g_set_error(error,
                    G_FILE_ERROR,
                    G_FILE_ERROR_NOMEM,
                    "Failed to allocate GKeyFile");
        return;
    }

    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, db_mem);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        g_key_file_set_string(
            keyfile,
            UDB_DB_SECTION,
            (const gchar *)key,
            (const gchar *)value);
    }

    gsize length = 0;
    gchar *data = g_key_file_to_data(keyfile, &length, error);
    if (data == NULL) {
        g_key_file_free(keyfile);
        return;
    }

    g_file_set_contents(
        db_file_path,
        data,
        length,
        error);

    g_free(data);
    g_key_file_free(keyfile);
}

void
db_load_from_file(GError **error)
{
    g_return_if_fail(db_file_path != NULL);

    GKeyFile *keyfile = g_key_file_new();
    if (!keyfile) {
        g_set_error(
            error,
            G_FILE_ERROR,
            G_FILE_ERROR_NOMEM,
            "Failed to allocate GKeyFile");
        return;
    }

    if (!g_key_file_load_from_file(
        keyfile,
        db_file_path,
        G_KEY_FILE_NONE,
        error))
    {
        g_key_file_free(keyfile);
        return;
    }

    g_hash_table_remove_all(db_mem);

    gsize n_keys = 0;
    gchar **keys = g_key_file_get_keys(
        keyfile,
        UDB_DB_SECTION,
        &n_keys,
        error);

    if (keys == NULL && n_keys == 0) {
        g_key_file_free(keyfile);
        return;
    }

    for (gsize i = 0; i < n_keys; i++) {
        const gchar *val = g_key_file_get_string(
            keyfile,
            UDB_DB_SECTION,
            keys[i],
            NULL);

        if (val) {
            g_hash_table_insert(
                db_mem,
                g_strdup(keys[i]),
                g_strdup(val));
        }
    }

    g_strfreev(keys);
    g_key_file_free(keyfile);
}

void
db_deinit() {
    g_mutex_clear(&db_mutex);
    g_hash_table_destroy(db_mem);
}

gboolean
db_insert(const char* key, const char* value)
{
    g_mutex_lock(&db_mutex);
    gboolean result = g_hash_table_insert(db_mem, g_strdup(key), g_strdup(value));
    g_mutex_unlock(&db_mutex);
    return result;
}

gchar*
db_lookup(const char* key)
{
    g_mutex_lock(&db_mutex);
    gchar* result = g_hash_table_lookup(db_mem, key);
    g_mutex_unlock(&db_mutex);
    return result;
}

gboolean
db_remove(const char* key) {
    g_mutex_lock(&db_mutex);
    gboolean result = g_hash_table_remove(db_mem, key);
    g_mutex_unlock(&db_mutex);
    return result;
}

static gboolean
on_db_save_timeout(gpointer user_data)
{
    UDB_UNUSED(user_data);
    GError* error = NULL;

    g_mutex_lock(&db_mutex);
    db_save_to_file(&error);
    g_mutex_unlock(&db_mutex);

    if (error) {
        g_printerr("[db-save] Error: %s\n", error->message);
        g_error_free(error);
    }

    return G_SOURCE_CONTINUE;
}

GScanner* udb_scanner;
GMutex scanner_mutex;

void
udb_scanner_init(void)
{
    g_mutex_init(&scanner_mutex);
   
    udb_scanner = g_scanner_new(NULL);
   
    udb_scanner->config->cset_skip_characters = " \t\r\n";
    
    udb_scanner->config->cset_identifier_first = G_CSET_a_2_z G_CSET_A_2_Z "_";
    udb_scanner->config->cset_identifier_nth = G_CSET_a_2_z G_CSET_A_2_Z G_CSET_DIGITS "_";
   
    udb_scanner->config->scan_identifier = TRUE;
    udb_scanner->config->scan_identifier_1char = TRUE;
   
    udb_scanner->config->scan_binary = TRUE;
    udb_scanner->config->scan_octal = TRUE;
    udb_scanner->config->scan_float = TRUE;
    udb_scanner->config->scan_hex = TRUE;
    
    udb_scanner->config->scan_string_sq = TRUE;
    udb_scanner->config->scan_string_dq = TRUE;
    
    udb_scanner->config->numbers_2_int = TRUE;
    udb_scanner->config->int_2_float = FALSE;
    
    udb_scanner->config->scan_comment_multi = FALSE;
    udb_scanner->config->skip_comment_single = FALSE;
    udb_scanner->config->skip_comment_multi = FALSE;
    udb_scanner->config->scan_hex_dollar = FALSE;
    udb_scanner->config->scan_symbols = FALSE;
    udb_scanner->config->symbol_2_token = FALSE;
    udb_scanner->config->char_2_token = FALSE;
}

gchar*
udb_handle_get(void)
{
    GTokenType varname_token = g_scanner_get_next_token(udb_scanner);
    gchar* key = NULL;

    switch (varname_token) {
    case G_TOKEN_IDENTIFIER:
        key = udb_scanner->value.v_identifier;
        break;
    case G_TOKEN_STRING:
        key = udb_scanner->value.v_string;
        break;
    default: return g_strdup_printf("ERR Missing KEY (token=%d)\r\n", varname_token);
    }

    gchar* result = db_lookup(key);
    
    if (!result) {
        return g_strdup_printf("NULL\r\n");
    }
    
    return g_strdup_printf("%s\r\n", result);
}

gchar*
udb_handle_set(void)
{
    GTokenType tA = g_scanner_get_next_token(udb_scanner);
    gchar* key = NULL;

    switch (tA) {
    case G_TOKEN_IDENTIFIER:
        key = udb_scanner->value.v_identifier;
        break;
    case G_TOKEN_STRING:
        key = udb_scanner->value.v_string;
        break;
    default: return g_strdup_printf("ERR Missing KEY (token=%d)\r\n", tA);
    }

    if (strlen(key) > UDB_MAX_KEY_LENGTH)
        return g_strdup_printf("ERR Key To Long\r\n");

    key = g_strdup(key);
   
    GTokenType tB = g_scanner_get_next_token(udb_scanner);
    gchar* value = NULL;
   
    switch (tB) {
        case G_TOKEN_IDENTIFIER:
            value = g_strdup(udb_scanner->value.v_identifier); // treat as string
            break;
        case G_TOKEN_INT:
            value = g_strdup_printf("%ld", udb_scanner->value.v_int);
            break;
        case G_TOKEN_FLOAT:
            value = g_strdup_printf("%g", udb_scanner->value.v_float);
            break;
        case G_TOKEN_STRING:
            value = g_strdup(udb_scanner->value.v_string);
            break;
        case G_TOKEN_EOF:
            g_free(key);
            return g_strdup_printf("ERR Missing Value Argument\r\n");
        default:
            g_free(key);
            return g_strdup_printf("ERR Malformed Value Argument (token=%d)\r\n", tB);
    }
   
    if (!value) {
        g_free(key);
        return g_strdup_printf("ERR NULL Value\r\n");
    }

    db_insert(key, value);
   
    g_free(key);
    g_free(value);
    return g_strdup_printf("OK\r\n");
}

gchar*
udb_handle_del(void)
{
    GTokenType varname_token = g_scanner_get_next_token(udb_scanner);
    gchar* key = NULL;

    switch (varname_token) {
    case G_TOKEN_IDENTIFIER:
        key = udb_scanner->value.v_identifier;
        break;
    case G_TOKEN_STRING:
        key = udb_scanner->value.v_string;
        break;
    default: return g_strdup_printf("ERR Missing KEY (token=%d)\r\n", varname_token);
    }

    db_remove(key); // returns true, if key actually existed. We don't care.

    return g_strdup_printf("OK\r\n");
}

gchar*
udb_handle_unknown(void)
{
    return g_strdup_printf("ERR Invalid Command\r\n");
}

typedef gchar* (*UDB_Handler)(void);
typedef struct {
    const char* name;
    UDB_Handler fn;
} UDB_CommandEntry;
static UDB_CommandEntry udb_commands[] = {
    { "GET", udb_handle_get },
    { "SET", udb_handle_set },
    { "DEL", udb_handle_del },
    { NULL, udb_handle_unknown },
};

static void
on_write_done (GObject      *source,
               GAsyncResult *res,
               gpointer      user_data)
{
    UDB_UNUSED(user_data);

    GOutputStream *out = G_OUTPUT_STREAM (source);
    GError *error = NULL;

    g_output_stream_write_finish (out, res, &error);

    if (error) {
        g_warning ("[write] Error: %s", error->message);
        g_clear_error (&error);
    }
}

gchar* 
process_command_line(const gchar* line)
{
    g_mutex_lock(&scanner_mutex);
    
    g_scanner_input_text(udb_scanner, line, -1);
    gchar* response = NULL;
    
    GTokenType tok = g_scanner_get_next_token(udb_scanner);
    if (tok != G_TOKEN_IDENTIFIER) {
        response = g_strdup_printf("ERR Expected Command Identifier (got token=%d)\r\n", tok);
        g_mutex_unlock(&scanner_mutex);
        return response;
    }
    
    const gchar* cmd = udb_scanner->value.v_identifier;
    gboolean command_found = FALSE;
    
    for (UDB_CommandEntry* e = udb_commands; e->name; ++e) {
        if (g_ascii_strcasecmp(cmd, e->name) == 0) {
            response = e->fn();
            command_found = TRUE;
            break;
        }
    }
    
    if (!command_found) {
        response = g_strdup_printf("ERR Unknown command: %s\r\n", cmd);
    }
    
    g_mutex_unlock(&scanner_mutex);
    return response;
}

static void
on_line_read (GObject      *source_object,
              GAsyncResult *res,
              gpointer      user_data)
{
    GDataInputStream* din = G_DATA_INPUT_STREAM (source_object);
    GSocketConnection* conn = G_SOCKET_CONNECTION (user_data);

    GError* error = NULL;
    gsize length = 0;
    gchar* line = g_data_input_stream_read_line_finish (din, res, &length, &error);

    if (error != NULL) {
        g_warning ("Read error: %s", error->message);
        g_clear_error (&error);
        g_object_unref (conn);
        return;
    }

    if (!line) { // EOF
        g_print("Client disconnected\n");
        g_object_unref(conn);
        return;
    }

    if (g_str_has_suffix(line, "\r")) // \r is not considered part of newline by `g_data_input_stream_read_line_async`
        line[strlen(line) - 1] = '\0';

    g_print("Received: %s\n", line);
    gchar* response = process_command_line(line);

    GOutputStream* out = g_io_stream_get_output_stream(G_IO_STREAM(conn));

    g_output_stream_write_async(
        out, response, strlen(response), G_PRIORITY_DEFAULT,
        NULL, on_write_done,
        NULL);

    g_free(response);
    g_free(line);

    g_data_input_stream_read_line_async(
        din, G_PRIORITY_DEFAULT, NULL, on_line_read, conn);
}

static gboolean
on_incoming (GSocketService    *service,
             GSocketConnection *connection,
             GObject           *source_object,
             gpointer           user_data)
{
    UDB_UNUSED(source_object);
    UDB_UNUSED(service);
    UDB_UNUSED(user_data);

    g_object_ref(connection);

    GInputStream* in = g_io_stream_get_input_stream(G_IO_STREAM(connection));
    GDataInputStream* din = g_data_input_stream_new(in);

    g_data_input_stream_read_line_async(din, G_PRIORITY_DEFAULT, NULL, on_line_read, connection);

    return TRUE;
}

static gboolean
on_check_sigint(gpointer user_data)
{
    UDB_UNUSED(user_data);
    if (!got_sigint) return G_SOURCE_CONTINUE;

    GError* error = NULL;

    g_mutex_lock(&db_mutex);
    db_save_to_file(&error);
    g_mutex_unlock(&db_mutex);

    if (error) {
        g_printerr("[db-save] Error: %s\n", error->message);
        g_error_free(error);
    }

    g_main_loop_quit(loop);
    return G_SOURCE_REMOVE;
}

int
main(int argc, char* argv[])
{
    GError* error = NULL;

    GOptionContext* opt_ctx = g_option_context_new(NULL);
    g_option_context_add_main_entries(opt_ctx, cmd_entries, NULL);
    if (!g_option_context_parse(opt_ctx, &argc, &argv, &error)) {
        g_printerr("%s: %s\n", argv[0], error->message);
        return error->code;
    }

    unlink(socket_path);
    signal(SIGINT, sigint_handler);

    GSocketService* socket_srvc = g_socket_service_new();
    if (!socket_srvc) {
        g_printerr("%s: %s\n", argv[0], error->message);
        return error->code;
    }

    GUnixSocketAddress* addr = G_UNIX_SOCKET_ADDRESS(g_unix_socket_address_new(socket_path));
    if (!g_socket_listener_add_address(
        G_SOCKET_LISTENER(socket_srvc),
        G_SOCKET_ADDRESS(addr),
        G_SOCKET_TYPE_STREAM,
        G_SOCKET_PROTOCOL_DEFAULT,
        NULL, NULL, &error)) {
        
        g_printerr("%s: %s\n", argv[0], error->message);
        return error->code;
    }

    g_signal_connect(socket_srvc, "incoming", G_CALLBACK(on_incoming), NULL);

    db_init();
    udb_scanner_init();

    if (db_file_path) {
        db_load_from_file(&error);
        if (error && error->code != G_FILE_ERROR_NOENT) { // if doesn't exist, just create it
            g_printerr("%s: (%u) %s\n", argv[0], error->code, error->message);
            return error->code;
        }

        g_timeout_add_seconds(
            UDB_DATABASE_SAVE_INTERVAL_SECS,
            on_db_save_timeout,
            NULL);
    }

    g_idle_add(on_check_sigint, NULL);

    g_socket_service_start(socket_srvc);
    g_print("Listening on %s\n", socket_path);

    loop = g_main_loop_new(NULL, FALSE);

    g_main_loop_run(loop);
    
    g_main_loop_unref(loop);
    g_mutex_clear(&db_mutex);
    g_hash_table_destroy(db_mem);

    return EXIT_SUCCESS;
}
