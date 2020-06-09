#!/bin/bash

# Create and upload firmware

./create_image.sh && curl --http1.0 -F myfile=@firmware.bin http://192.168.1.200/

