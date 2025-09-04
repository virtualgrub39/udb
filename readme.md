# UDB

Simple Key-Value database over UNIX Socket

## Why?

This branch was created, so that I can try working with POSIX API directly, without any fancy wrappers. Now I know, why people use fancy wrappers...

## Building

This branch is POSIX only. You need < 20yo. compiler running on linux/bsd machine. That's it. 

To build the executable, you just have to run make:

```bash
make all
```

In the project root directory.

Running this command will also copy config.def.h to config.h. It can be used to customize some behavior of the server.

## Usage

### Executable usage:

```bash
./udb <options>
```

### Server usage (protocol):

#### Example client code snippet:

```c
char *buffer1 = "SET \"best vocaloid\" \"Hatsune Miku\"\r\n";
char* buffer2 = "GET \"best vocaloid\"\r\n";

int sock = socket (AF_UNIX, SOCK_SEQPACKET, 0);

struct sockaddr_un addr = {
    .sun_family = AF_UNIX,
    .sun_path = "/tmp/udb.sock", // modify to actual socket path
};
connect (sock, (struct sockaddr *)&addr, sizeof (addr));

char recv_buffer[16] = { 0 };

write (sock, buffer1, strlen (buffer1));
read (sock, recv_buffer, sizeof(recv_buffer));
printf ("%s", recv_buffer); // OK\r\n

write (sock, buffer2, strlen (buffer2));
read (sock, recv_buffer, sizeof(recv_buffer));
printf ("%s", recv_buffer); // "Hatsune Miku"\r\n

close(sock);
```

#### Commands:

- `GET <variable name>\r\n` - server will reply with the value stored in `variable name` or with `NULL` if not set.
- `SET <variable name> <value>\r\n` - store `value` into `variable name`. Server will reply with error message or `OK`.
- `DEL <variable name>\r\n` - delete value from `variable name`. Server will reply with deleted variable value, or `NULL` if it was not set.

`variable name` must be a string (quoted) or identifier (unquoted, not starting with a number).

`value` can be anything, though everything is stored as string internally.

#### Server replies:

- All server replies end with `\r\n`
- On general success, server will reply with `OK\r\n`
- Server may reply with `ERR <error message>\r\n` to `SET` command, if there was an error while executing the command. The error details are contained in `error message`.

## License

See [LICENSE](LICENSE)
