// radio_sim.h
//
// Simulated radio back end: no SDR hardware, TX samples are looped straight
// back into the receiver (through the same decimating filter the Pluto path
// uses) with optional noise and a real time sample clock.
//
#ifndef RADIO_SIM_H
#define RADIO_SIM_H

#include <stdbool.h>

void sim_open(void);
void sim_close(void);
void sim_transmit_samples(scmplx *s, int len);
int  sim_receive_samples(scmplx *s, int len);   // returns samples produced
double sim_get_level(void);                     // 0..1 pseudo signal strength
void sim_set_loopback(bool on);
void sim_set_noise(double level);               // 0..0.5
void sim_set_realtime(bool on);
bool sim_get_loopback(void);
double sim_get_noise(void);
bool sim_get_realtime(void);
bool sim_is_keyed(void);
void sim_set_tx_gain(double g);
void sim_set_rx_gain(double g);
double sim_get_tx_gain(void);
double sim_get_rx_gain(void);

#endif // RADIO_SIM_H
