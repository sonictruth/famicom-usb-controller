// Famicom joypad-1 emulation: OUT0 strobes, each /OE1 (read of $4016) shifts.
#pragma once
void padEmuBegin();   // configure pins
void padEmuRun();     // tight polling loop on the calling core; never returns
