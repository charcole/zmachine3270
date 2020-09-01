#!/bin/bash

# Create and upload firmware

echo -n LGV_DATA > GameDataWithHeader.bin
cat GameData.bin >> GameDataWithHeader.bin
curl --http1.0 -F myfile=@GameDataWithHeader.bin http://192.168.1.201/game.html

