#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include "m17defines.h"
#ifdef HAVE_SIM
#include "radio_sim.h"
#endif

void help(void){
	gui_help();
}
//
// Pad a Null terminated address into a 9 character address
//
void pad_address(const char *ain, char *aout){
	int n = strlen(ain);
	if( n > 9 )
	memset(aout,' ',9);
	for( int i = 0; i < n; i++){
		aout[i] = toupper(ain[i]);
	}
}
// Keep the chosen mode for the next start.  config.txt holds the same commands
// the console takes, one per line, so its "mode" line is replaced in place.
// Writing a temporary file and renaming it means a failure part way through
// cannot leave a half written config behind.
static void save_mode_to_config(const char *mode){
	char line[256];
	const char *tmp = "config.txt.tmp";
	FILE *in  = fopen("config.txt","r");
	FILE *out = fopen(tmp,"w");
	bool wrote = false;
	if(out == NULL){
		printf("m17web: cannot write config.txt, the mode will not be kept\n");
		return;
	}
	if(in != NULL){
		while(fgets(line, sizeof(line), in) != NULL){
			char *p = line;
			while(*p == ' ' || *p == '\t') p++;
			if(strncmp(p,"mode",4) == 0 && (p[4] == ' ' || p[4] == '\t')){
				if(wrote == false){
					fprintf(out, "mode %s\n", mode);
					wrote = true;
				}
				continue;                       // the old line is dropped
			}
			fputs(line, out);
		}
		fclose(in);
	}
	if(wrote == false) fprintf(out, "mode %s\n", mode);   // not there: add it
	fclose(out);
	if(rename(tmp, "config.txt") != 0)
		printf("m17web: could not replace config.txt\n");
	else
		printf("m17web: mode %s kept for the next start\n", mode);
}

void mmi_parse(char *cmd){
	int nt = 0;
	bool valid = false;
	char *token[10];

	// Drop trailing line endings and whitespace: the tokeniser only splits on
	// spaces, so "radio\n" from config.txt used to become a bad token value.
	int cl = (int)strlen(cmd);
	while(cl > 0 && (cmd[cl-1] == '\n' || cmd[cl-1] == '\r' ||
	                 cmd[cl-1] == ' '  || cmd[cl-1] == '	'))
		cmd[--cl] = 0;
	if(cl == 0) return;

	token[nt] = strtok(cmd," ");
	while(token[nt] != NULL){
		if( ++nt == 10 ) break;
		token[nt] = strtok(NULL," ");
	}

	// Start of commands

	// Comment
	if(strncmp(token[0],"#",1)==0){
		valid = true;;
	}

	// Transmitter Gain
	if(strncmp(token[0],"tg",2)==0){
		if(	nt == 2 ){
			float gain = atof(token[1]);
			if((gain <= 1.0)&&(gain>=0)){
				radio_set_tx_gain(gain);
				valid = true;
			}
		}
	}

	// Receiver Gain
	if(strncmp(token[0],"rg",2)==0){
		if(	nt == 2 ){
			float gain = atof(token[1]);
			if((gain <= 1.0)&&(gain>=0)){
				radio_set_rx_gain(gain);
				valid = true;
			}
		}
	}

	// Transmitter frequency
	if(strncmp(token[0],"tf",2)==0){
		if(	nt == 2 ){
			uint64_t freq = atoll(token[1]);
			radio_set_tx_frequency(freq);
			m17_db_set_tx_freq(freq);
			valid = true;
		}
	}

	// Receiver frequency
	if(strncmp(token[0],"rf",2)==0){
		if(	nt == 2 ){
			uint64_t freq = atoll(token[1]);
			radio_set_rx_frequency(freq);
			m17_db_set_rx_freq(freq);
			valid = true;
		}
	}

	// Frequency correction factor
	if(strncmp(token[0],"fc",2)==0){
		if(	nt == 2 ){
			double factor = atof(token[1]);
			radio_set_freq_correction_factor(factor);
			valid = true;
		}
	}

	// Automatic Frequency Control
	if(strncmp(token[0],"afc",3)==0){
		if(	nt == 2 ){
			if(strncmp(token[1],"on",2)==0){
				radio_set_afc_on();
				valid = true;
			}else{
				if(strncmp(token[1],"off",3)==0){
					radio_set_afc_off();
					valid = true;
				}
			}
		}
	}

	// Quit
	if(strncmp(token[0],"q",1)==0){
		m17_txrx_disable();
	}

	// Transmit carrier
	if(strncmp(token[0],"tc",2)==0){
		m17_txrx_ptt_carrier();
		valid = true;
	}

	// Transmit
	if(strncmp(token[0],"tx",2)==0){
		m17_txrx_ptt_on();
		valid = true;
	}

	// Duplex
	if(strncmp(token[0],"td",2)==0){
		m17_txrx_ptt_duplex();
		valid = true;
	}

	// Receive
	if(strncmp(token[0],"rx",2)==0){
		m17_txrx_ptt_off();
		valid = true;
	}

	// Source Address
	if(strncmp(token[0],"sa",2)==0){
		char text[10];
		pad_address(token[1], text);
		m17_txrx_set_src_add(text);
		valid = true;
	}

	// Gateway address
	if(strncmp(token[0],"ga",2)==0){
		char text[10];
		pad_address(token[1], text);
		char c = token[2][0];
		text[8] = c;
		m17_txrx_set_gate_add(text);
		valid = true;
	}
	// Destination Address
	if(strncmp(token[0],"da",2)==0){
		char text[10];
		pad_address(token[1], text);
		m17_txrx_set_dest_add(text);
		valid = true;
	}
	// Broadcast destination Address
	if(strncmp(token[0],"ba",2)==0){
		m17_txrx_set_brd_add();
		valid = true;
	}

	// Position log: csv on|off|status, or the interval in seconds
	if(strncmp(token[0],"csv",3)==0){
		if(nt == 1 || strncmp(token[1],"st",2)==0){
			printf("CSV position log is %s, every %d s, %d rows\n",
			       gps_csv_on() ? "on" : "off", gps_csv_interval(), gps_csv_rows());
			printf("  file %s\n\r", gps_csv_path());
		} else if(strncmp(token[1],"on",2)==0 || strncmp(token[1],"off",3)==0){
			bool on = (strncmp(token[1],"on",2)==0);
			gps_csv_set(on, nt > 2 ? atoi(token[2]) : 0);
			printf("CSV position log %s%s\n\r", on ? "on, every " : "off",
			       on ? "" : "");
			if(on) printf("CSV position log file %s\n\r", gps_csv_path());
		} else {
			gps_csv_set(gps_csv_on(), atoi(token[1]));      // just a new interval
			printf("CSV position log every %d s\n\r", gps_csv_interval());
		}
		valid = true;
	}

	// GPS receiver: status, or pick a different device / speed
	if(strncmp(token[0],"gps",3)==0){
		if(nt == 1){
			char st[160], grid[12];
			gps_status_text(st, sizeof(st));
			gps_maidenhead(grid, sizeof(grid));
			printf("GPS %s at %d baud\n", gps_device()[0] ? gps_device() : "(auto-detect)", gps_baud());
			printf("  status   %s\n", gps_status());
			printf("  position %s  locator %s\n", st, grid);
			printf("  altitude %d m\n", (int)m_gps.alt);
			printf("  sent in META %s\n", m17_tx_gnss() ? "yes" : "no");
			printf("  CSV log %s (%d rows in %s)\n\r",
			       gps_csv_on() ? "on" : "off", gps_csv_rows(), gps_csv_path());
		} else {
			gps_close();
			gps_set_device(token[1], nt > 2 ? atoi(token[2]) : 0);
			gps_open();
			printf("GPS reopened on %s\n\r", token[1]);
		}
		valid = true;
	}

	// Blank frames sent before the voice, in codec frames (2 = 40 ms)
	if(strncmp(token[0],"lead",4)==0){
		if(nt == 2){
			m17_tx_set_lead_frames(atoi(token[1]));
			valid = true;
		}
	}

	//   radio  - RF in, local audio out (the normal voice mode)
	//   loop   - the audio is looped back inside the program, for practice
	//   gate   - the radio is bridged to a reflector
	//   client - local audio is bridged to a reflector
	// A mode may be changed while running: the thread for the old mode stands
	// down (see m17_txrx_mode_changed) and the dispatcher starts the new one.
	// The choice is written to config.txt so it survives a restart.
	if(strncmp(token[0],"mode",4) == 0 ){
		CircuitType want = m17_db_get_chan_type();
		bool known = true;
		if(strncmp(token[1],"loop",4) == 0)        want = ASTOAS;
		else if(strncmp(token[1],"radio",5) == 0)  want = DRTOAS;
		else if(strncmp(token[1],"gate",4) == 0)   want = DRTODN;
		else if(strncmp(token[1],"client",6) == 0) want = ASTODN;
		else known = false;

		if(!known){
			gui_cmd_resp("mode: radio, loop, gate or client");
			valid = true;
		} else {
			bool was_net = (m17_db_get_chan_type() == DRTODN || m17_db_get_chan_type() == ASTODN);
			bool is_net  = (want == DRTODN || want == ASTODN);
			gui_save_mode(token[1]);

			if(want != m17_db_get_chan_type()){
				if(was_net && !is_net){
					// Leaving a network mode: leave the reflector politely first
					m17_net_disconnect_from_reflector();
					m17_net_close();
				}
				m17_db_set_chan_type(want);
				m17_txrx_mode_changed();
				save_mode_to_config(token[1]);
			}
			if(is_net) m17_net_open();      // needed at startup and when entering

			char text[80];
			snprintf(text, sizeof(text), "mode %s", token[1]);
			printf("m17web: %s\n", text);
			fflush(stdout);
			gui_cmd_resp(text);
			valid = true;
		}
	}

	if(strncmp(token[0],"w",1)==0){
		gui_update_clear();
		valid = true;
	}

	if(strncmp(token[0],"conn",4)==0){
		if(nt == 3){
			char c = token[2][0];
			// The UDP sockets used to be opened only by the gate/client modes,
			// so "connect" in the normal radio mode sent nothing at all.
			m17_net_open();
            m17_net_connect_to_reflector( token[1], m17_db_get_src(), c);
		    valid = true;
		}
	}
	if(strncmp(token[0],"disc",4)==0){
		m17_net_disconnect_from_reflector();
		valid = true;
	}

	// Audio routing: alsa (Pi sound card, direct), pulse (Pi sound card via
	// PulseAudio), browser (web UI), file, both, none.
	// "audio alsa plughw:3,0" also selects the ALSA device.
	if(strncmp(token[0],"audio",5)==0){
		if(nt == 2 || nt == 3){
			if(nt == 3) audio_alsa_set_device(token[2]);
			audio_set_backend(audio_parse_backend(token[1]));
			valid = true;
		}
	}

	// Listen to all addresses (not just ours and the broadcast address)
	if(strncmp(token[0],"la",2)==0){
		if(nt == 2){
			bool on = (strncmp(token[1],"on",2)==0)||(strncmp(token[1],"all",3)==0);
			m17_txrx_listen_all(on);
			valid = true;
		}
	}

	// Stop transmitting
	if(strncmp(token[0],"b",1)==0){
		m17_txrx_ptt_off();
		valid = true;
	}

#ifdef HAVE_SIM
	// Simulated radio controls: sim loop on|off, sim realtime on|off, sim noise 0.01
	if(strncmp(token[0],"sim",3)==0){
		if(nt == 3){
			if(strncmp(token[1],"loop",4)==0)
				sim_set_loopback(strncmp(token[2],"on",2)==0);
			if(strncmp(token[1],"realtime",8)==0)
				sim_set_realtime(strncmp(token[2],"on",2)==0);
			if(strncmp(token[1],"noise",5)==0)
				sim_set_noise(atof(token[2]));
			valid = true;
		}
	}
#endif

	// Help
	if(strncmp(token[0],"h",1)==0){
		help();
		valid = true;
	}

	if(valid == false){
		gui_cmd_resp("Invalid command");
	}
	else{
		gui_cmd_resp("OK");
	}
	gui_update();
}
void mmi_load_file(const char *name){
	FILE *fp;
	char text[256];
	if((fp=fopen(name,"r"))!=NULL){
		while(fgets(text,255,fp)!= NULL){
			//printf("%s",text);
			mmi_parse(text);
		}
	}else{
		char text[1024];
		sprintf(text,"Unable to open %s\n",name);
		gui_cmd_resp(text);
	}
}
