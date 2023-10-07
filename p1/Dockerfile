FROM ubuntu:22.04

RUN apt update
RUN apt install build-essential strace valgrind -y --no-install-recommends


ADD *.c *.h *.sh Makefile /app/

WORKDIR /app

RUN "make"

ENTRYPOINT [ "./run.sh"]
