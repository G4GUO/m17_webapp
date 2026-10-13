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

	// These commands are only valid at startup
	if(strncmp(token[0],"mode",4) == 0 ){
		gui_save_mode(token[1]);

		if(strncmp(token[1],"loop",4) == 0 ){
		    // Audio Loopback
		    m17_db_set_chan_type(ASTOAS);
			valid = true;
	    }
	    if(strncmp(token[1],"radio",5) == 0 ){
		    // Digital Radio to Local Audio
		    // Microphone to Digital Radio
		    m17_db_set_chan_type(DRTOAS);
			valid = true;
	    }
	    if(strncmp(token[1],"gate",4) == 0 ){
		    // RF Gateway
		    m17_db_set_chan_type(DRTODN);
	        // Open network
	        m17_net_open();
			valid = true;
	    }
	    if(strncmp(token[1],"client",6) == 0 ){
		    // Audio Gateway
		    m17_db_set_chan_type(ASTODN);
	        // Open network
	        m17_net_open();
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
