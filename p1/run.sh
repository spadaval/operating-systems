#!/bin/sh
echo "==========================="
echo "Evaluating expression $1:"
./cs238 $1
echo "\n\n\n"
echo "==========================="
echo "Running strace:"
echo "==========================="
strace ./cs238 $1
echo "\n\n\n"
echo "==========================="
echo "Running valgrind:"
echo "==========================="
valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes ./cs238 $1
