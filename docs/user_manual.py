#!/usr/bin/env python3
"""Build the m17web user manual PDF.

    python3 docs/user_manual.py [out.pdf]

The text lives in this file so the manual can be edited and regenerated without
any other tooling.  Layout is plain reportlab platypus; table cells are
Paragraphs so long text wraps inside fixed column widths.
"""
from __future__ import annotations

import sys
from datetime import date

from reportlab.graphics.shapes import Drawing, Line, Rect, String
from reportlab.lib import colors
from reportlab.lib.enums import TA_CENTER
from reportlab.lib.pagesizes import A4
from reportlab.lib.styles import ParagraphStyle, getSampleStyleSheet
from reportlab.lib.units import cm
from reportlab.platypus import (
    BaseDocTemplate,
    HRFlowable,
    Frame,
    ListFlowable,
    ListItem,
    PageBreak,
    PageTemplate,
    Paragraph,
    Spacer,
    Table,
    TableStyle,
)

TITLE = "m17web User Manual"
SUBTITLE = "M17 digital voice on a Raspberry Pi with an SDR, driven from your web browser"
VERSION = "Version 1.0 \u2014 " + date.today().strftime("%d %B %Y")

MARGIN = 2.7 * cm                 # ~87 characters per line: comfortable for body text
FRAME_W = A4[0] - 2 * MARGIN      # usable width between the margins

_ss = getSampleStyleSheet()
S = {
    "title": ParagraphStyle("t", parent=_ss["Title"], fontSize=26, leading=30, spaceAfter=6),
    "subtitle": ParagraphStyle("st", parent=_ss["BodyText"], fontSize=12.5, leading=17,
                               textColor=colors.HexColor("#444444"), alignment=TA_CENTER,
                               spaceAfter=4),
    "version": ParagraphStyle("v", parent=_ss["BodyText"], fontSize=10, leading=14,
                              textColor=colors.HexColor("#666666"), alignment=TA_CENTER),
    "h1": ParagraphStyle("h1", parent=_ss["Heading1"], fontSize=16, leading=20,
                         spaceBefore=16, spaceAfter=7, textColor=colors.HexColor("#12395c")),
    "h2": ParagraphStyle("h2", parent=_ss["Heading2"], fontSize=12.5, leading=16,
                         spaceBefore=11, spaceAfter=5, textColor=colors.HexColor("#2a5c86")),
    "p": ParagraphStyle("p", parent=_ss["BodyText"], fontSize=10.5, leading=15, spaceAfter=7),
    "li": ParagraphStyle("li", parent=_ss["BodyText"], fontSize=10.5, leading=15, spaceAfter=3),
    "note": ParagraphStyle("n", parent=_ss["BodyText"], fontSize=9.5, leading=13.5,
                           textColor=colors.HexColor("#555555"), spaceAfter=8),
    "cell": ParagraphStyle("c", parent=_ss["BodyText"], fontSize=9.5, leading=12.5, spaceAfter=0),
    "cellh": ParagraphStyle("ch", parent=_ss["BodyText"], fontSize=9.5, leading=12.5,
                            spaceAfter=0, fontName="Helvetica-Bold"),
    "code": ParagraphStyle("code", parent=_ss["BodyText"], fontSize=9.5, leading=13,
                           fontName="Courier", leftIndent=10, spaceAfter=6),
    "caption": ParagraphStyle("cap", parent=_ss["BodyText"], fontSize=9, leading=12.5,
                              textColor=colors.HexColor("#555555"), spaceBefore=4, spaceAfter=10),
}


class Manual:
    def __init__(self) -> None:
        self.story: list = []

    # -- building blocks ----------------------------------------------------
    def h1(self, text: str) -> None:
        self.story.append(Paragraph(text, S["h1"]))

    def h2(self, text: str) -> None:
        self.story.append(Paragraph(text, S["h2"]))

    def p(self, text: str) -> None:
        self.story.append(Paragraph(text, S["p"]))

    def note(self, text: str) -> None:
        self.story.append(Paragraph(text, S["note"]))

    def code(self, text: str) -> None:
        self.story.append(Paragraph(text, S["code"]))

    def bullets(self, items: list[str]) -> None:
        self.story.append(ListFlowable(
            [ListItem(Paragraph(i, S["li"]), leftIndent=14, value="bullet") for i in items],
            bulletType="bullet", start="\u2022", leftIndent=14, bulletFontSize=9,
            spaceBefore=2, spaceAfter=8,
        ))

    def steps(self, items: list[str]) -> None:
        self.story.append(ListFlowable(
            [ListItem(Paragraph(i, S["li"]), leftIndent=18) for i in items],
            bulletType="1", leftIndent=18, bulletFontSize=9.5,
            spaceBefore=2, spaceAfter=8,
        ))

    def table(self, rows: list[list[str]], widths: list[float], header: bool = True) -> None:
        data = []
        for r_i, row in enumerate(rows):
            style = S["cellh"] if (header and r_i == 0) else S["cell"]
            data.append([Paragraph(str(c), style) for c in row])
        total = sum(widths)
        cols = [w / total * FRAME_W for w in widths]
        t = Table(data, colWidths=cols, repeatRows=1 if header else 0)
        style = [
            ("GRID", (0, 0), (-1, -1), 0.4, colors.HexColor("#9aa5ae")),
            ("VALIGN", (0, 0), (-1, -1), "TOP"),
            ("LEFTPADDING", (0, 0), (-1, -1), 5),
            ("RIGHTPADDING", (0, 0), (-1, -1), 5),
            ("TOPPADDING", (0, 0), (-1, -1), 4),
            ("BOTTOMPADDING", (0, 0), (-1, -1), 4),
        ]
        if header:
            style.append(("BACKGROUND", (0, 0), (-1, 0), colors.HexColor("#e8eef4")))
        t.setStyle(TableStyle(style))
        self.story.append(t)
        self.story.append(Spacer(1, 10))

    def pagebreak(self) -> None:
        self.story.append(PageBreak())

    # -- the layout sketch --------------------------------------------------
    def sketch(self) -> None:
        """A labelled sketch of the panel layout - not a screenshot."""
        W, H = FRAME_W, 250
        d = Drawing(W, H)
        dark, light, edge = colors.HexColor("#12395c"), colors.HexColor("#eef3f7"), colors.HexColor("#8fa3b5")
        d.add(Rect(0, H - 24, W, 24, fillColor=dark, strokeColor=dark))
        d.add(String(8, H - 17, "m17web", fontName="Helvetica-Bold", fontSize=11, fillColor=colors.white))
        for i, label in enumerate(["radio", "mode", "audio", "clients", "online"]):
            d.add(String(90 + i * 68, H - 17, label, fontName="Helvetica", fontSize=8,
                         fillColor=colors.HexColor("#cfe0ee")))

        def card(x, y, w, h, title, lines=()):
            d.add(Rect(x, y, w, h, fillColor=light, strokeColor=edge))
            d.add(String(x + 6, y + h - 13, title, fontName="Helvetica-Bold", fontSize=9, fillColor=dark))
            for i, t in enumerate(lines):
                d.add(String(x + 6, y + h - 25 - i * 11, t, fontName="Helvetica", fontSize=7.5,
                             fillColor=colors.HexColor("#44586b")))

        pad, top = 6, H - 32
        cw = (W - 3 * pad) / 2.0
        card(pad, top - 118, cw, 112, "HOLD TO TALK", [
            "RX / TX light    signal: in frame", "signal \u25af\u25af\u25af\u25af\u25af  power \u25af\u25af\u25af",
            "break     carrier     duplex",
        ])
        card(pad, top - 208, cw, 84, "Station", [
            "my callsign        destination", "gateway            listen to all",
            "broadcast (ba)   help   clear log",
        ])
        card(pad + cw + pad, top - 118, cw, 112, "Radio", [
            "RX frequency (Hz)   TX frequency (Hz)", "RX gain   TX gain   freq correction",
            "apply frequencies   transmit carrier", "receive      noise",
        ])
        card(pad + cw + pad, top - 208, cw, 84, "Reflector / Audio / Console", [
            "reflector   module   connect / disconnect",
            "audio device   blank frames before voice",
            "volume   mute   enable browser audio",
            "Console: type a command here ... [send]",
        ])
        self.story.append(d)
        self.story.append(Paragraph(
            "Figure 1: the panels on the page. The header is always visible; PTT, Station and Radio "
            "on the left, Radio, Reflector, Audio and Console on the right. (Sketch, not a screenshot.)",
            S["caption"]))


def build(path: str) -> None:
    m = Manual()
    ed = m.story.append

    # ---------------------------------------------------------------- cover
    ed(Spacer(1, 4.2 * cm))
    m.h1("")   # ensure the title style block sits after the spacer
    m.story.pop()
    ed(Paragraph(TITLE, S["title"]))
    ed(Paragraph(SUBTITLE, S["subtitle"]))
    ed(Spacer(1, 0.6 * cm))
    ed(Paragraph(VERSION, S["version"]))
    ed(Spacer(1, 1.6 * cm))
    m.p("<b>This manual is for anyone using the radio.</b> It explains what the program does, "
        "how to start it, how to make a contact, and what to do when something does not work. "
        "You do not need to understand M17, SDR or Linux to use it \u2014 if someone set the system up "
        "for you, start at section 4.")
    m.p("A separate README in the same folder covers the technical details: building the program, "
        "the audio back ends, and the transmit timing.")
    m.h2("What is in this manual")
    m.table([
        ["", "Setting it up", "", "Using it"],
        ["1", "What m17web does", "7", "Audio: where your voice comes from and goes"],
        ["2", "What you need", "8", "The controls, panel by panel"],
        ["3", "Starting and stopping it", "9", "Commands"],
        ["4", "Opening the interface", "10", "If something goes wrong"],
        ["5", "The screen at a glance", "11", "Good practice"],
        ["6", "Making a contact", "12", "Quick reference"],
    ], [0.4, 2.9, 0.4, 4.3])
    m.pagebreak()

    # ----------------------------------------------------------- 1. what it is
    m.h1("1. What m17web does")
    m.p("M17 is a digital voice mode for amateur radio: like FM voice, but it sends your speech as "
        "data, which gives clear audio and carries your callsign with it. m17web is a program that "
        "runs on a small computer (a Raspberry Pi) connected to a software-defined radio. It sends "
        "and receives M17 voice, and you control it entirely from a web page \u2014 no terminal "
        "commands needed in normal use.")
    m.p("Your voice comes from a microphone plugged into the Pi, or from the computer you are "
        "browsing from. Received voice comes out of the Pi's speaker (or your computer's).")
    m.note("If you are only practising, the program can run without a radio, in a mode that feeds "
           "your own audio straight back. See section 9.")

    # ------------------------------------------------------------ 2. what you need
    m.h1("2. What you need")
    m.table([
        ["Item", "Notes"],
        ["Raspberry Pi 4", "Any 4 GB or 8 GB model, with a network connection. The Pi runs m17web."],
        ["ADALM-Pluto (PlutoSDR / Pluto+)", "The radio itself, connected to the Pi by USB. A LimeSDR also works."],
        ["USB sound card with microphone and speaker or headphones",
         "For audio at the Pi. Recommended - it is the most reliable audio path."],
        ["Antenna and a valid amateur licence", "You need a licence for any frequency you transmit on."],
        ["A web browser", "On any computer, tablet or phone on the same network as the Pi."],
    ], [2.6, 5.4])

    # --------------------------------------------------------- 3. start and stop
    m.h1("3. Starting and stopping it")
    m.p("m17web is started from a terminal on the Pi. If someone else set the system up for you, it "
        "may already be running \u2014 in that case go straight to section 4.")
    m.p("<b>Start it</b> (this also works after a reboot):")
    m.code("cd /mnt/hf/shared/projects/m17_sdr_web/m17gismo<br/>"
           "RADIO=pluto MODE=radio bash tools/pi_m17web.sh start -a alsa")
    m.p("It keeps running after you close the terminal window. <b>Check it:</b>")
    m.code("bash tools/pi_m17web.sh status")
    m.p("<b>Stop it</b>, or look at the log to see what it is doing:")
    m.code("bash tools/pi_m17web.sh stop<br/>bash tools/pi_m17web.sh log")
    m.h2("What the settings mean")
    m.table([
        ["Setting", "What it does"],
        ["RADIO=pluto", "Use the PlutoSDR. Use <b>RADIO=sim</b> instead to practise with no radio attached."],
        ["MODE=radio", "Radio in, local sound out \u2014 the normal voice mode."],
        ["-a alsa", "Send the voice to the Pi's sound card (see section 7). Use <b>-a browser</b> to "
                    "use the browser's own microphone and speakers instead."],
    ], [1.7, 6.3])

    # ------------------------------------------------------------- 4. the page
    m.h1("4. Opening the interface")
    m.p("In a browser on the same network, go to the address of the Pi followed by the port number:")
    m.code("http://10.168.1.33:8080/")
    m.p("Substitute the address of your own Pi. Bookmark it: this is the whole interface.")
    m.p("At the top right there is an <b>online / offline</b> marker. If it says offline, the page has "
        "lost contact with the Pi and nothing you press will do anything \u2014 check that the program "
        "is running (section 3) and reload the page.")
    m.pagebreak()

    m.h1("5. The screen at a glance")
    m.sketch()
    m.table([
        ["Area", "What it is for"],
        ["HOLD TO TALK", "Press and hold while you speak, then release. You can hold the space bar instead."],
        ["RX / TX light", "Shows whether you are transmitting. Next to it, <b>in frame</b> means a valid "
                          "M17 signal is being decoded, <b>no signal</b> means nothing is there."],
        ["signal and power bars", "How strong the received signal is, and your transmit power setting."],
        ["Station", "Your callsign, who you are calling, and the broadcast and log buttons."],
        ["Radio", "The frequencies and gain settings, and the button that applies them."],
        ["Reflector", "Linking over the internet to other M17 users (you may never need it)."],
        ["Audio", "Where your voice comes from and goes, the volume, and how healthy your transmit audio is."],
        ["Console", "A command box for advanced settings, and the log that shows what is happening."],
    ], [1.9, 6.1])

    # -------------------------------------------------------- 6. making a contact
    m.h1("6. Making a contact")
    m.steps([
        "Type your callsign into <b>Station \u2192 my callsign</b>. It is sent with every transmission, "
        "so it must be correct.",
        "Agree a frequency with the other station, then type it into <b>Radio \u2192 RX frequency (Hz)</b> "
        "and press <b>apply frequencies</b>. This sets transmit to the same frequency.",
        "Listen first. When the other station speaks, the light shows <b>RX</b>, the signal bar moves, "
        "<b>in frame</b> appears and you hear them.",
        "To reply, hold <b>HOLD TO TALK</b>, speak in your normal voice a little slower than a "
        "telephone call, then release.",
        "Wait for the other station to finish before you press again \u2014 while you are transmitting, "
        "you cannot hear them.",
    ])
    m.note("Only transmit on frequencies your licence allows, and check what is already in use before "
           "you press. If you are testing, a dummy load or the lowest gain keeps you out of trouble.")

    # ----------------------------------------------------------------- 7. audio
    m.h1("7. Audio \u2014 where your voice comes from and goes")
    m.p("Set this once in the <b>Audio</b> panel; it stays set.")
    m.table([
        ["Option", "What it means", "When to use it"],
        ["Raspberry Pi sound card (direct)",
         "Uses the microphone and speaker plugged into the Pi. Talks to the sound card directly.",
         "<b>Recommended.</b> The most reliable choice."],
        ["browser microphone / speaker",
         "Uses the microphone and speakers of the computer you are browsing from, sent over the network.",
         "When you are away from the Pi, or the Pi has no sound card. See the note below."],
        ["Raspberry Pi sound card (PulseAudio)",
         "The same card, but through the PulseAudio sound service.",
         "Only if something else on the Pi needs to share the card. Direct is more reliable."],
        ["browser + Pi sound card", "Both at once, listening in two places.", "Rarely needed."],
        ["mute (no audio)", "Silence going out and coming in.", "For testing."],
    ], [2.0, 4.2, 1.8])
    m.p("A change takes effect at the start of your next transmission; the panel shows "
        "<b>(applying...)</b> until then.")
    m.h2("If the browser refuses your microphone")
    m.p("Browsers only allow microphone access on a secure page. A page opened as "
        "<b>http://&lt;address&gt;:8080/</b> is not secure, so the microphone may be blocked. Either "
        "open <b>http://localhost:8080/</b> in a browser on the Pi itself, or use the Pi's sound card "
        "option instead \u2014 which avoids the problem completely and is what most users want.")
    m.h2("Blank frames before voice, and the transmit health line")
    m.p("Next to the audio device there are two things worth knowing about.")
    m.p("<b>blank frames before voice</b> is the amount of silence sent at the start of each "
        "transmission, before your speech. It gives the receiving station time to lock onto your "
        "signal, so your first word is not clipped. The default is 4 (about 80 thousandths of a "
        "second). If people tell you the start of your first word is missing, set it to 6 or 8.")
    m.p("The line beside it \u2014 <b>transmit clean</b>, or <b>dropped N samples</b> \u2014 is the health of "
        "your transmit audio. <b>Transmit clean</b> means the Pi kept up with the radio for the whole "
        "transmission. If it reports dropped samples, raise the blank frames a little and try again; "
        "if it keeps reporting drops, the Pi is too busy (close other programs) or something needs "
        "looking at by whoever maintains the system.")
    m.p("<b>volume</b> and <b>mute</b> affect only what you hear in the browser. The small "
        "<b>mic</b> and <b>spk</b> bars show the audio levels going out and coming in \u2014 the mic bar "
        "should move when you speak.")
    m.pagebreak()

    # ------------------------------------------------------ 8. the panels in detail
    m.h1("8. The controls, panel by panel")
    m.h2("Station")
    m.table([
        ["Control", "What it does"],
        ["my callsign", "Your callsign. Sent with every transmission."],
        ["destination", "Who you are calling. Leave as ALL unless you are calling one specific station."],
        ["gateway", "Used when linking through a reflector or repeater (advanced)."],
        ["listen to all", "Receive everything on the frequency, whoever it is addressed to."],
        ["broadcast (ba)", "Address the transmission to everyone (the usual choice on a calling channel)."],
        ["help / clear log", "Lists every command in the log, or empties the log."],
    ], [1.9, 6.1])

    m.h2("Radio")
    m.table([
        ["Control", "What it does"],
        ["RX frequency (Hz)", "The frequency you listen on, in hertz (145500000 = 145.5 MHz)."],
        ["TX frequency (Hz)", "The frequency you transmit on. Usually the same as RX."],
        ["RX gain / TX gain", "Receive and transmit levels. Leave them as they are unless someone asks you to change them."],
        ["freq correction", "Fine tuning, for when a station sounds slightly off. Change it in small steps."],
        ["apply frequencies", "Press this after changing a frequency or a gain, or nothing changes."],
        ["transmit carrier", "Sends a plain carrier with no voice - for tests and alignment only."],
        ["receive", "Returns to receiving if a transmission is stuck on, or after a test."],
        ["noise", "Practice mode only: adds noise to the simulated signal."],
    ], [2.0, 6.0])

    m.h2("Reflector")
    m.p("Reflectors link M17 stations over the internet, so you can talk to people out of radio "
        "range. Choose a reflector from the list, choose the channel letter (the <b>module</b>), and "
        "press <b>connect</b>. Press <b>disconnect</b> when you have finished \u2014 it is good manners, "
        "and it frees the channel for others.")
    m.note("Reflector use may need the gateway address filled in as well, and a working internet "
           "connection on the Pi. If you only want local radio contacts, ignore this panel.")

    m.h2("Audio")
    m.table([
        ["Control", "What it does"],
        ["audio device", "Where your voice comes from and goes - see section 7."],
        ["blank frames before voice", "Silence sent before your speech at the start of a transmission. Default 4."],
        ["enable browser audio", "Must be pressed once (and permission given) before the browser can play or record audio."],
        ["volume / mute", "What you hear in the browser."],
        ["mic / spk bars", "Audio levels going out and coming in."],
    ], [2.3, 5.7])

    m.h2("Console")
    m.p("Every setting also has a typed command, and the console is where you type them. You do not "
        "need it for normal operation; the help button lists the commands, and section 9 lists them "
        "all. The log below it shows what the program is doing \u2014 useful when something does not "
        "work.")

    # ----------------------------------------------------------- 9. the commands
    m.h1("9. Commands")
    m.p("Typed into the Console box, or into the program's own terminal. Type one command per line.")
    m.table([
        ["Command", "What it does"],
        ["rf 145500000", "Set the receive frequency, in hertz."],
        ["tf 145500000", "Set the transmit frequency, in hertz."],
        ["rg 0.5 / tg 0.5", "Set the receive or transmit gain, from 0.0 to 1.0."],
        ["fc 1.0001", "Frequency correction, from 0.9 to 1.1."],
        ["afc on / off", "Automatic frequency control."],
        ["tx / rx", "Start or stop transmitting (the PTT button does the same)."],
        ["td / tc", "Transmit duplex, or transmit a plain carrier."],
        ["sa G4ABC", "Source address: your callsign."],
        ["da ALL", "Destination address: who you are calling."],
        ["ga G4ABC G", "Gateway address."],
        ["ba", "Broadcast: call everyone."],
        ["la on / off", "Listen to all addresses, or only yours."],
        ["conn NAME C", "Connect to a reflector, on module C."],
        ["disc", "Disconnect from the reflector."],
        ["mode radio", "Normal mode: radio in, sound out. Alternatives: <b>loop</b> (audio loops back for "
                       "practice), <b>gate</b> (radio to reflector), <b>client</b> (sound to reflector)."],
        ["audio alsa", "Voice through the Pi sound card. Alternatives: <b>browser</b>, <b>pulse</b>, "
                       "<b>both</b>, <b>none</b>."],
        ["lead 4", "Blank codec frames sent before the voice - see section 7."],
        ["load FILE", "Run a list of commands from a file."],
        ["w", "Clear the log pane."],
        ["b", "Break: stop transmitting now."],
        ["q", "Quit the program."],
        ["h", "List the commands."],
    ], [1.7, 6.3])

    # --------------------------------------------------------- 10. troubleshooting
    m.h1("10. If something goes wrong")
    m.table([
        ["What you see", "What to try"],
        ["The page will not open at all",
         "Check the address and the port (8080), and that the Pi is switched on and on the network. "
         "On the Pi, run <b>bash tools/pi_m17web.sh status</b>."],
        ["It says <b>offline</b> at the top",
         "The page lost contact with the Pi. Reload the page. If it stays offline, check the program "
         "is still running on the Pi."],
        ["Nothing comes out of the speaker",
         "In Audio, make sure the Pi's sound card is selected, the volume is up and mute is off, and "
         "that the speaker is plugged into the sound card (not the Pi's headphone socket)."],
        ["The other station says my audio breaks up, or starts late",
         "Look at the transmit health line in the Audio panel. Raise <b>blank frames before voice</b> "
         "to 6-8 and try again. If it still reports dropped samples, the Pi is struggling with "
         "something else running."],
        ["I hear nothing when the other station speaks",
         "Check you are on the same frequency, and that <b>in frame</b> appears when they talk. If the "
         "signal bar moves but there is no audio, try <b>afc on</b>, then nudge <b>freq correction</b> "
         "slightly."],
        ["The browser will not use my microphone",
         "Browsers only allow the microphone on a secure page. Use <b>http://localhost:8080/</b> on "
         "the Pi itself, or select the Pi's sound card instead (section 7)."],
        ["The light never goes to TX",
         "If the header says offline, the page cannot reach the Pi. Otherwise something else may be "
         "using the radio \u2014 stop and start the program."],
        ["The sound card is reported as busy",
         "Another program (or an earlier run) is using it. Stop other audio programs, then stop and "
         "restart m17web."],
        ["The program will not start (port already in use)",
         "An older copy is still running: <b>bash tools/pi_m17web.sh stop</b>, wait a moment, then "
         "start it again."],
        ["The radio cannot be found at all",
         "The Pluto re-appears as a USB network interface when it is plugged in, and it needs the "
         "address 192.168.2.2. Check it is connected, then ask whoever maintains the system \u2014 this "
         "is covered in the README."],
    ], [2.1, 5.9])

    # ------------------------------------------------------------ 11. good practice
    m.h1("11. Good practice")
    m.bullets([
        "Transmit only on frequencies your licence covers, and at the lowest power that works.",
        "Listen before you transmit, and give way to a station already in contact.",
        "Keep your callsign correct in the Station panel \u2014 it is how other operators know who is calling.",
        "When using a reflector, say where you are going and disconnect when you have finished.",
        "For tests, use a dummy load or a carrier into a low gain rather than transmitting into the antenna.",
        "If you leave the system running, mute the audio if you do not want your room heard on the air.",
    ])

    # --------------------------------------------------------- 12. quick reference
    m.pagebreak()
    m.h1("12. Quick reference")
    m.h2("Start it")
    m.code("cd /mnt/hf/shared/projects/m17_sdr_web/m17gismo<br/>"
           "RADIO=pluto MODE=radio bash tools/pi_m17web.sh start -a alsa")
    m.h2("Open it")
    m.code("http://10.168.1.33:8080/     (your own Pi's address)")
    m.h2("Make a contact")
    m.table([
        ["Step", "Do this"],
        ["1", "Station → my callsign: type your callsign."],
        ["2", "Radio → RX frequency (Hz): type the agreed frequency, press <b>apply frequencies</b>."],
        ["3", "Listen until you hear the other station (<b>in frame</b> appears)."],
        ["4", "Hold <b>HOLD TO TALK</b>, speak, release."],
        ["5", "Let the other station finish before pressing again."],
    ], [0.9, 7.1])
    m.h2("The three things that put most people right")
    m.table([
        ["If", "Then"],
        ["You cannot hear anything",
         "Check the frequency, and whether <b>in frame</b> appears when the other station speaks. "
         "Try <b>afc on</b>."],
        ["Nobody can hear you",
         "Look at the transmit line in the Audio panel. If it reports dropped samples, raise "
         "<b>blank frames before voice</b> to 6-8."],
        ["The page will not reach the Pi",
         "On the Pi: <b>bash tools/pi_m17web.sh status</b>, then reload the page."],
    ], [2.3, 5.7])
    m.note("Settings worth knowing: <b>audio device</b> (use the Pi's sound card, direct), "
           "<b>blank frames before voice</b> (default 4) and <b>volume</b> in the Audio panel; "
           "<b>help</b> in the Station panel lists every command.")

    # --------------------------------------------------------------- 13. credits
    m.h1("13. Where this came from, and further reading")
    m.p("m17web is a modification of <b>G4GUO/m17_sdr</b>, with the original terminal interface "
        "replaced by the web page described here. The M17 protocol it speaks is an open standard "
        "from the M17 Project; the voice coding is Codec2 by David Rowe and the M17 Project's "
        "Codec2-mod build. It uses the open-source libiio, ALSA and Codec2 libraries.")
    m.p("For the technical details \u2014 building it yourself, the audio back ends, the transmit "
        "timing and the rate planning \u2014 see <b>README.md</b> next to this manual, in the same folder.")
    m.note("The upstream project has no licence file, so this fork adds its own documentation only. "
           "Treat the software as you find it: it works, but check it against your own licence "
           "conditions before putting it on the air.")

    # ------------------------------------------------------------------ notes
    m.pagebreak()
    m.h1("Notes")
    m.p("Frequencies in use locally, reflector names, who to call when it breaks:")
    for _ in range(18):
        m.story.append(Spacer(1, 0.42 * cm))
        m.story.append(HRFlowable(width="100%", thickness=0.4,
                                  color=colors.HexColor("#c8d2dc"), spaceAfter=0))

    # ------------------------------------------------------------------ render
    doc = BaseDocTemplate(path, pagesize=A4, title=TITLE, author="m17web project",
                      leftMargin=MARGIN, rightMargin=MARGIN, topMargin=1.8 * cm,
                      bottomMargin=1.8 * cm)

    def on_page(canv, _doc):
        canv.saveState()
        canv.setStrokeColor(colors.HexColor("#c8d2dc"))
        canv.setLineWidth(0.4)
        y = A4[1] - 1.35 * cm
        canv.line(MARGIN, y, A4[0] - MARGIN, y)
        canv.setFont("Helvetica", 8)
        canv.setFillColor(colors.HexColor("#77808a"))
        canv.drawString(MARGIN, y + 4, TITLE)
        canv.drawRightString(A4[0] - MARGIN, y + 4, "M17 digital voice over SDR")
        canv.line(MARGIN, 1.35 * cm, A4[0] - MARGIN, 1.35 * cm)
        canv.drawCentredString(A4[0] / 2.0, 1.0 * cm, "Page %d" % canv.getPageNumber())
        canv.restoreState()

    frame = Frame(doc.leftMargin, doc.bottomMargin, doc.width, doc.height, id="body")
    doc.addPageTemplates([PageTemplate(id="all", frames=[frame], onPage=on_page)])
    doc.build(m.story)
    print("wrote", path)


if __name__ == "__main__":
    build(sys.argv[1] if len(sys.argv) > 1 else "docs/m17web-user-manual.pdf")
