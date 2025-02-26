compile:
	cd build && make

upload: compile
	cd build && picotool load -F test.uf2 && picotool reboot -f

reboot:
	picotool reboot -f

clean:
	cd build && make clean

monitor:
	 minicom -b 115200 -o -D  /dev/ttyACM0

init:
	echo "Read character Manually Create your own Project at /home/pi/Bookshelf/getting-started-with-pico.pdf" 

