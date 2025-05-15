# UDB

Simple Key-Value database over UNIX Socket

## Why?

I've created this project to get familiar with UNIX sockets, GLib library and async IO using GIO.

## Building

Glib is required. Also, makefile makes use of pkg-config to find the Glib.

I've only created makefile for linux, but glib supposedly compiles on windows too.

To build the executable, you just have to run make:

```bash
make all
```

In the project root directory.

Running this command will also create config.h file, that can be used to customize some behavior of the server.

## Usage

### Executable usage:

```bash
./udb <options>
```

#### Options:
- `--socket-path`, `-p` - specify the path to unix socket the server will listen on. The default value can be modified in `config.h` by changing `UDB_SOCKET_PATH_DEFAULT` definition.
- `--db-file`, `-f` - provide the path to file, where database state will be saved. `NULL` by default (database doesn't persist it's state by default).

### Server usage (protocol):

#### Example client code snippet:

```c
char* buffer = "SET \"best vocaloid\" \"Hatsune Miku\"\r\nGET \"best vocaloid\"\r\n";

int sock = socket(AF_UNIX, SOCK_STREAM, 0);

struct sockaddr_un addr = {
    .sun_family = AF_UNIX,
    .sun_path = "/tmp/udb.sock", // modify to actual socket path
}
connect(sock, (struct sockaddr*)&addr, sizeof(addr));

write(sock, buffer, sizeof(buffer));

char recv_buffer[14] = { 0 };

read(sock, recv_buffer, 4);
printf("%s", recv_buffer); // OK\r\n
read(sock, recv_buffer, 14);
printf("%s", recv_buffer); // Hatsune Miku\r\n
```

You also use:

```bash
socat - UNIX-CONNECT:<path to socket>
```
as testing client.

#### Commands:

- `GET <variable name>\r\n` - server will reply with the value stored in `variable name` or with `NULL` if not set.
- `SET <variable name> <value>\r\n` - store `value` into `variable name`. Server will reply with error message or `OK`.
- `DEL <variable name>\r\n` - delete value from `variable name`. Server will reply with `OK`, even if the value of `variable name` was never set.

`variable name` must be either a valid C identifier (as defined by GScanner - tbh I'm not even sure what GScanner considers valid.) or a valid **ACII** string, encaplsulated by double quotes. Example:

- *VALID COMMANDS*: `GET "fancy var name 123"`; `SET my_waifu420 "what am I even doing right now"`
- *INVALID COMMANDS*: `SET 420isafunnynumber 69isalsoafunnynumber`

`value` can be anything, though everything is stored as UTF8 string internally. It is generally advised to encapsulate the value in double quotes, as the parser handles it better.

#### Server replies:

- All server replies end with `\r\n`
- On general success, server will reply with `OK\r\n`
- Server may reply with `ERR <error message>\r\n` to `SET` command, if there was an error while executing the command. The error details are contained in `error message`.

## License

```
Author: © 2025 virtualgrub39 <virtualgrub39(at)tutamail.com>
SPDX-License-Identifier: MIT-0

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the “Software”), to use,
copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
the Software, and to permit persons to whom the Software is furnished to do
so, without restriction.

THE SOFTWARE IS PROVIDED “AS IS”, WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.

```
