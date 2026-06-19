# ESP-IDF build system
# Prerequisites: set IDF_PATH or run export.ps1 from ESP-IDF installation

PORT ?= $(shell python3 board_detect.py 2>/dev/null)

default:
	idf.py build

upload:
	idf.py -p "$(PORT)" flash

monitor:
	idf.py -p "$(PORT)" monitor

flash:
	idf.py -p "$(PORT)" flash monitor

menuconfig:
	idf.py menuconfig

clean:
	idf.py fullclean

set-target:
	idf.py set-target

format:
	astyle --options="formatter.conf" --recursive "main/*.cpp" "main/*.h"
