# Conway's Game of Life for SEGA Genesis

This is Conway's game of life built using the SGDK for the SEGA Genesis/MegaDrive.

This project was built in a matter of just several minutes using Claude AI (Haiku 4.6).
I did not want to take credit for programming this so I wanted to be transparent about
how I built this project. It appears stable and usable, but if you find issues, feel
free to post an issue on this repository and I will try to fix it.

## Controls

A / B / C - Toggle the fill of a cell
Start - Pause or Resume simulation

## Game notes

The grid is fixed and unable to expand or move. If a new cell is to generate outside
of the grid, it will not be considered. This means that once a particular moving 
element reaches the screen, it will not move off of the screen. 

## Building

As previously mentioned, this project was built using the SGDK. In order to build
this project, you must first follow the steps to set up the SGDK.

Once you have the SGDK compiled on your system, you can build the project by 
navigating to the location of this repository in your console and running the following 
command:

```bash
<SGDK_LOCATION>\bin\make -f <SGDK_LOCATION>\makefile.gen
```

Where `<SGDK_LOCATION>` refers to the path of where your SGDK repository that you have
built exists on your system. Note that the use of back-slashes (`\`) in my path are due
to the fact that I am building on a Windows machine using the command line. For Linux
or Mac OS users, you would use forward slashes (`/`) for your path in the command line.

I will post builds as releases in this repository so you do not have to make your own 
build, but if you would like to edit the program and need any help with building then
please let me know and I can give you a hand.

## Thank you

I appreciate you checking out this goofy little project. I'm very passionate about the
SEGA Genesis and I was pretty happy with this, so I figured to share it. If you have 
any suggestions you are free to make them in the issues section of this repo. Also, 
happy recent 35th birthday Sonic the Hedgehog. I love you!
