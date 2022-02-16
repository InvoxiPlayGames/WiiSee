# WiiSee

A BrainSlug module to allow for sending the display output from a Nintendo Wii game to another device on the network, such as a PC.

## Performance

Currently, the performance is very poor, due to attempting to send the entire Wii framebuffer through the Wii's lackluster network card. This is helped slightly by implementing basic compression (currently miniLZO), but this adds extra CPU load. I've only been able to get roughly 0.7 frames per second sent to the PC while in-game on Mario Kart Wii on a Wii U over Wi-Fi. The end goal of this project is to be somewhere within the ballpark of 10 to 30 fps. This doesn't necessarily have to be realtime nor high resolution - delay and quality reduction is acceptable so long as the output is consistent.

|Task (performed on Wii U, on Mario Kart Wii, in a local race)|Time taken|
|---|---|
|Latency from frame render to network code|~25 milliseconds (set in code)|
|Time from first packet sent to last packet sent (includes compression)|~500-700 milliseconds|
|Time from first packet received to last packet received|~1500-2000 milliseconds|

## Usage

This project isn't intended for use by the general public yet. For developers, you will want a development environment capable of compiling BrainSlug modules, and familiarity with using them. You'll also want to create a file called "network.h" with the following contents:

```c
#define CLIENT_PORT 1569 // the port used by the Wii
#define SERVER_IP 0xc0a80020 // the hex representation of your listening server's IP address (this is 192.168.0.32)
#define SERVER_PORT 1569 // the port your listening server is listening on
```

## Protocol

The current protocol "version" is 0x00. This is by no means finalised, and there is no true protocol documentation due to the project changing. Check the current source code to see how packets are sent to the PC.

## License

This project is licensed under the [GNU General Public License v2](https://www.gnu.org/licenses/old-licenses/gpl-2.0.html).

This project uses miniLZO: https://www.oberhumer.com/opensource/lzo/

This project uses code from GeckodNET: https://code.google.com/archive/p/geckowii/