// ui_help.cpp
//
// The command help text, shared by the web and console front ends.
//
#include <stddef.h>

static const char *const help_lines[] = {
	"rf nnnnnnnnn    set receive frequency in Hz",
	"tf nnnnnnnnn    set transmit frequency in Hz",
	"rg 0.0 - 1.0    receive gain",
	"tg 0.0 - 1.0    transmit gain",
	"fc 0.9 - 1.1    frequency correction factor",
	"afc on | off    automatic frequency control",
	"tx              transmit (PTT on, simplex)",
	"td              transmit duplex",
	"tc              transmit carrier",
	"rx              receive (PTT off)",
	"sa xxxxxxxxx    source address / your callsign",
	"ga xxxxxxxxx    gateway address",
	"da xxxxxxxxx    destination address",
	"ba              destination broadcast",
	"la on | off     listen to all addresses",
	"conn NAME MOD   connect to reflector NAME module MOD",
	"disc            disconnect from the reflector",
	"mode loop       audio loopback",
	"mode radio      M17 radio <-> local audio",
	"mode gate       M17 radio <-> reflector gateway",
	"mode client     local audio <-> reflector",
	"audio browser   voice via the web browser (default)",
	"audio pulse     voice via the local sound card",
	"audio both      browser plus local sound card",
	"audio none      mute",
	"agi nnnn        configure audio input gain (dB)",
	"load FILE       run commands from FILE",
	"w               clear the log pane",
	"b               break: stop transmitting",
	"q               quit m17web",
	"h               this help",
	NULL
};

const char *const *ui_help_lines(void) { return help_lines; }
