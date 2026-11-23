# yash-shell

A small Unix shell written in C. It supports running commands, a single pipe,
basic input/output/error redirection, background jobs, and the `jobs`, `fg`,
and `bg` built-ins.

## Requirements

- A Unix-like environment
- GCC and Make
- GNU Readline development files

On Ubuntu or Debian, install the build dependencies with:

```sh
sudo apt update
sudo apt install build-essential libreadline-dev
```

## Build

From the project directory, run:

```sh
make
```

This builds the `yash` executable.

## Run

Start the shell with:

```sh
./yash
```

Enter commands at the `#` prompt. Press **Ctrl-D** to exit.

Examples (put spaces around operators):

```sh
ls -l
cat input.txt | grep hello
sort < input.txt > output.txt
command 2> errors.txt
sleep 10 &
```

Use `jobs` to list jobs, `fg` to bring a job to the foreground, and `bg` to
resume a stopped job in the background. Press **Ctrl-C** to interrupt a
foreground job or **Ctrl-Z** to stop it.

To remove the executable and object file, run:

```sh
make clean
```
