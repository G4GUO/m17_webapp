// m17_mailbox.h
//
// File mailboxes for the AI link.
//
// Two directories replace the audio WebSocket for an assistant:
//
//   inbox   the receiver writes one WAV per over here; the assistant transcribes
//           it and deletes it
//   outbox  the assistant writes a reply here; the station polls, keys, plays it,
//           unkeys and deletes it
//
// Nothing depends on a socket staying open and nothing has to guess a rate, so
// the two directions cannot interfere with each other.

#ifndef M17_MAILBOX_H
#define M17_MAILBOX_H

void m17_mailbox_set(const char *inbox, const char *outbox);   // "" turns it off
bool m17_mailbox_enabled(void);
bool m17_mailbox_busy(void);        // a reply is being transmitted

void m17_mailbox_aos(void);         // start recording the over into the inbox
void m17_mailbox_los(void);         // finish that file
void m17_mailbox_service(void);     // poll the outbox

#endif
