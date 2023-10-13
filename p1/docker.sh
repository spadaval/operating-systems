#!/bin/bash
docker build . -t app && docker run --rm app 1+1