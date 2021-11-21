#!/bin/sh

set -xe

gcc binplay.c -o binplay -lportaudio -Wall -O3
