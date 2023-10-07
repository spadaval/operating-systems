FROM debian

RUN apt update
RUN apt install build-essential -y --no-install-recommends

ADD *.c .
ADD *.h .
ADD Makefile .

RUN "make"

ENTRYPOINT [ "./cs238" ]
