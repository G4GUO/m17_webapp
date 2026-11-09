# SMS over M17

Text messages ride in the same M17 packet burst as AX.25, so everything already
known about getting a packet across applies unchanged.  What differs is the
application data type and, on receive, who the message is for.

## On the wire

| Item | Value |
|---|---|
| Data type specifier | **0x05** (M17 spec Table 3.15, "SMS", null terminated UTF-8) |
| Application data | type byte + the message + a NUL terminator |
| Limit | 823 bytes of application data (spec 2.0.10 §2.9) = **821 characters** |
| Burst | 2 preambles, LSF x3, the packet frame(s), 3 tails - identical to AX.25 |
| Framing | 25-byte chunks, EOF and count in the 6-bit metadata, CRC-16 over the whole data |
| Source | the LSF source address, which for a single burst is the originator |

The CRC covers the type specifier as well as the message, and the frame builder
appends it - the application block stops at the NUL.

A message longer than 821 characters is **truncated**, not refused: the page
slices at the limit, the station slices again before queueing, and the encoder
slices once more.  Three independent cuts means no path can produce an oversized
packet, and a long message still arrives in part instead of not at all.

## Why it is not the same as AX.25

AX.25 carries the calling stations inside its own header, so the LSF source is
only "whoever keyed the burst" and does not matter.  An SMS has no header of its
own, so the LSF source **is** the sender - that is the callsign shown in the web
page.  A relayed SMS would therefore show the relay rather than the originator;
the specification's answer to that is the extended-callsign META field, which
this implementation does not yet use.

## Transmit path

`m17_sms_queue()` (m17_sms.cpp) puts the text on a four-deep queue and returns at
once.  `m17_sms_service()` is then called from the **transmitter thread**, next
to `m17_ax25_service()`, and builds `0x05 + text + NUL` for
`m17_send_app_burst()` in m17_ax25.cpp.

That placement is not cosmetic: the first version gave SMS a thread of its own
and the station segfaulted inside the first burst, on both stations, because the
modulator, the transmit sample buffer and the Pluto's buffers all belong to the
transmitter thread.  Anything that keys the radio has to run there.

`m17_send_app_burst()`:

* takes the **burst lock** shared with AX.25, so an SMS and a frame can never
  interleave on air;
* refuses to key while voice is transmitting (`-2`), and the queue entry is put
  back rather than dropped;
* honours `loopback`, in which case it builds and reassembles the burst and hands
  it to `valid_packet_received()` - so the whole receive path, decoder included,
  is exercised with no radio at all.

## Receive path

`valid_packet_received()` (m17_rx_parse.cpp) dispatches on the type byte: 0x05
goes to `m17_sms_rx_packet()`, everything else goes to the AX.25/KISS path
unchanged.  The message is stored in a ring of 32 with the time it was heard and
the station that sent it, and pushed to every open page as:

```json
{"t":"sms","time":"15:46:30","src":"G4GUO","text":"LOOPBACK WITH SOURCE"}
```

## Headless use

Everything works without a browser:

```
sms <text>      queue a message; it goes out from the transmitter thread
sms             list the last five heard, newest first
```

Received messages print as they arrive, on stdout and in the journal:

```
m17web: RX  SMS  [G4GUO-2] B TO A ATTEMPT 1
```

The brackets are there because the callsign is not part of the message - without
them a log reads as though the sender's call were the first word of the text.
There is no listener of any kind: the only ways in are the existing web
websocket (8080) and the command line.

### Logging what is received

An unattended station's journal rotates, so every received message can also be
appended to a plain file:

```
smslog              state, and where it is writing
smslog on | off     turn it on or off; kept for the next start
smslog file PATH    write somewhere other than sms.log (in the station's own
                    directory, next to config.txt)
```

One line per message, with the date as well as the time because the file outlives
any one session:

```
2026-10-31 16:24:21 [G4GUO-2] LOGGED FROM B 1
```

Off by default.  Sent messages are not logged - the point is a record of what the
station heard.

## Web interface

The SMS card sits under the command help: a list above (time, bracketed source,
text, newest first - five of them) and one input line with a send button.  Enter
in the box transmits.  The card is warm brown rather than the page's blue, so it
reads as its own thing.
The hint counts characters against the streamed limit, which the server sends in
its greeting as `sms_max`, so the page and the station cannot disagree.

The send button uses its own message type (`{"t":"sms","text":"..."}`) rather
than the `sms <text>` command: the command channel is cut at about 250 characters
and a message may be 821.

## Testing without a radio

```
loopback on
sms TEST MESSAGE
```

which prints `TX SMS`, then `LB SMS ... loopback`, then `RX SMS ... from <your
call>` and adds the message to the card.  On air between two stations the source
is the other station's callsign.

## Notes

* `sms` alone reports how many are held and the maximum length; the console form
  `sms <text>` still works, subject to the command-line limit.
* Reserved for the Android app: the message shape above is the whole protocol, so
  a client only needs to send `0x05 + text + NUL` in a packet burst and read the
  same back with the LSF source as the sender.
