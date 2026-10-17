/*
 * ============================================================================
 *  PICO W WI-FI "HAYES MODEM"  (fully annotated edition)
 * ============================================================================
 *
 *  THE BIG PICTURE
 *  ---------------
 *  Your ThinkPad 365XD thinks it is talking to a modem. A real modem would
 *  turn bytes into screechy audio and push them down a phone line. This
 *  firmware does a trick instead: it listens for modem commands on a serial
 *  wire, and when the laptop says "dial", it opens a TCP connection over
 *  Wi-Fi and passes bytes back and forth. The laptop never knows the
 *  difference, because the *serial side* of a modem is a text protocol.
 *
 *      [ThinkPad COM1] --RS-232 (+-12V)--> [MAX3232] --3.3V--> [Pico UART0]
 *                                                                   |
 *                                                  this program, on the Pico W
 *                                                                   |
 *                                                          Wi-Fi -> internet
 *
 *  A LITTLE HISTORY: RS-232
 *  ------------------------
 *  RS-232 was published by the Electronic Industries Association around 1960
 *  (the famous "-C" revision came in 1969). It was designed for exactly one
 *  job: connecting a TERMINAL (a screen and keyboard, called the DTE, "Data
 *  Terminal Equipment") to a MODEM (called the DCE, "Data Communication
 *  Equipment") so the terminal could talk over phone lines to a mainframe.
 *  Your laptop is a DTE. This Pico is pretending to be a DCE. That's why
 *  the wiring is "straight through" - DTE pin 3 (transmit) lands on the
 *  DCE's receive.
 *
 *  Electrically, RS-232 is odd. It's a *bipolar* signal:
 *      logic 1 ("mark")  = a NEGATIVE voltage, -3V to -15V (usually about -12V)
 *      logic 0 ("space") = a POSITIVE voltage, +3V to +15V (usually about +12V)
 *  An idle line sits at "mark" (negative). The words mark and space come
 *  from telegraphs. A byte is sent as: one START bit (space), 8 data bits
 *  (least-significant first), then one STOP bit (mark). That's "8N1": 8 data
 *  bits, No parity, 1 stop bit, and it's what we configure below.
 *
 *  The Pico speaks 0V/3.3V logic and would be damaged by +-12V, which is
 *  why the MAX3232 chip sits between them as a level shifter.
 *
 *  The PC's original serial chip was the 8250 (1981 IBM PC), which had a
 *  1-byte buffer. The 16550 (late 1980s, probably what's in your laptop) added a
 *  16-byte FIFO so the CPU didn't have to service every character. The Pico's
 *  UART (an ARM PL011) has 32-byte FIFOs.
 *
 *  A LITTLE HISTORY: THE MODEM AND DIAL-UP
 *  ---------------------------------------
 *  The phone network only carries audio of roughly 300 Hz to 3400 Hz. Modems
 *  ("MOdulator-DEModulator") encode bits as tones in that band. Early
 *  standards: Bell 103 (1960s, 300 bits/sec), Bell 212A (1200), then V.32,
 *  V.32bis, and V.34 in 1994 (28,800 bits/sec - your NetSurf card). V.90
 *  (1998) hit 56k by exploiting digital phone trunks. Note "baud" (symbols
 *  per second) is not the same as bits per second once a modem packs several
 *  bits into each symbol, though people use them interchangeably.
 *
 *  Dial-up as we know it: you pick up the line (go "off-hook"), wait for
 *  dial tone, send number tones (DTMF "touch-tone", or old rotary-style
 *  pulses), the far modem answers with a high-pitched tone, and the two
 *  squeal at each other to agree on a speed. Once they agree, the modem
 *  reports CONNECT. If the far end drops, the "carrier" tone disappears and
 *  you get NO CARRIER. We imitate these exact messages below.
 *
 *  THE HAYES COMMAND SET
 *  ---------------------
 *  In 1981 Hayes Microcomputer Products (Dennis Hayes and Dale Heatherington)
 *  released the Smartmodem. Before it, you had to physically dial a phone
 *  and flip a switch. The Smartmodem accepted text commands over the same
 *  serial line that carried data, all starting with "AT" ("ATtention").
 *      ATDT5551212   = ATtention, Dial using Tone, number 5551212
 *      ATH           = Hang up
 *      ATZ           = Reset
 *      +++           = "escape" back to command mode while connected
 *  The escape sequence needs a pause (a "guard time", about one second)
 *  before and after the three plus signs, so that a file that happens to
 *  contain "+++" doesn't hang up the call. Everyone copied the Hayes set,
 *  which is why Windows 95, Telix and every BBS program speak it.
 *
 *  A LITTLE HISTORY: BBSes and TELNET
 *  ----------------------------------
 *  The first dial-up BBS (Bulletin Board System) went online in Chicago in
 *  1978. You called them with a modem, and today many survive over the
 *  internet via TELNET (RFC 854, 1983), a simple text protocol over TCP.
 *  Telnet has an in-band control scheme: byte 255 ("IAC", Interpret As
 *  Command) says "the next bytes are a command, not text". We handle that
 *  below so you don't see junk characters on screen.
 * ============================================================================
 */

#include <stdio.h>                  // printf() for debug output over USB serial
#include <string.h>                 // strlen/strcmp/strncpy/strrchr/memcpy helpers
#include <strings.h>                // strncasecmp(): case-insensitive compare (AT commands may be lowercase)
#include <stdlib.h>                 // atoi(): turn the port number text into an integer
#include "pico/stdlib.h"            // Pico SDK basics: sleep_ms(), gpio, time functions
#include "pico/cyw43_arch.h"        // Wi-Fi chip (CYW43) driver glue, includes lwIP integration
#include "hardware/uart.h"          // Pico hardware UART API (our serial port to the laptop)
#include "lwip/tcp.h"               // lwIP raw TCP API (the TCP/IP stack running on the Pico)
#include "lwip/dns.h"               // lwIP DNS resolver (turns "bbs.example.com" into an IP)

#ifndef WIFI_SSID                   // if CMake didn't pass the Wi-Fi network name...
#error "Pass -DWIFI_SSID=... and -DWIFI_PASSWORD=... to cmake"   // ...stop the build with a helpful message
#endif                              // end of the SSID check

#define MODEM_UART    uart0         // use the Pico's first hardware UART for the laptop link
#define MODEM_TX_PIN  0             // GP0 = Pico transmit, goes to MAX3232 T1IN
#define MODEM_RX_PIN  1             // GP1 = Pico receive, comes from MAX3232 R1OUT
#define MODEM_BAUD    19200         // serial speed (bits/sec). Must match the laptop's COM port setting

#define RING_SIZE 16384             // size of the receive buffer in bytes (must be a power of two)
#define RING_MASK (RING_SIZE - 1)   // bit-mask to wrap an index around the buffer cheaply (index & mask)

/*
 * PHONE BOOK
 * Windows 95 Dial-Up Networking and many old programs only accept digits in
 * the "phone number" box, so we map fake phone numbers to real internet
 * hosts. Dialing "1" really connects to the host on the right.
 */
static const struct {               // an array of {number, target} pairs, fixed at compile time
    const char *number;             // what you "dial" on the laptop
    const char *target;             // the host:port we really connect to
} phonebook[] = {                   // start of the table
    {"1",       "bbs.fozztexx.com:23"},       // example telnet BBS (verify it's still up)
    {"2",       "towel.blinkenlights.nl:23"}, // example ASCII-art telnet service (verify too)
    {"5551212", "example.com:80"},            // a plain web server, for poking at HTTP by hand
};                                  // end of table

/* ---------------------------- global state ------------------------------ */

static uint8_t ring[RING_SIZE];     // circular buffer holding bytes received from the internet
static volatile uint32_t ring_head; // where the network callback writes next ("volatile": changed from interrupt-ish context)
static volatile uint32_t ring_tail; // where the main loop reads next (head - tail = bytes waiting)
static uint32_t ack_pending;        // bytes we've drained to the UART but not yet told TCP about (window update)

static struct tcp_pcb *pcb;         // the current TCP connection ("protocol control block"), NULL if none
static volatile bool connected;     // true once the TCP handshake completes
static volatile bool remote_closed; // true when the far side hangs up or the connection errors
static volatile bool dns_done;      // true when a DNS lookup has finished (success or fail)
static volatile bool dns_ok;        // true if the DNS lookup found an address
static ip_addr_t resolved;          // the IP address DNS gave us

static bool data_mode;              // false = command mode (listening for AT...), true = passing data through
static bool echo = true;            // echo typed characters back, like a real modem's default (ATE1)
static bool telnet_filter = true;   // strip/refuse telnet IAC sequences (turn off with ATNET0 for raw TCP)

static uint8_t txbuf[256];          // small staging buffer: bytes from laptop waiting to go to the internet
static size_t txlen;                // how many bytes are in txbuf right now

/* ---------------------------- UART helpers ------------------------------ */

static void uputs(const char *s) {                          // send a C string to the laptop
    while (*s) uart_putc_raw(MODEM_UART, *s++);             // push each char out the serial port, no CR/LF translation
}                                                           // end uputs

static void result(const char *s) {                         // print a modem "result code" like OK or CONNECT
    uputs("\r\n");                                          // carriage return + line feed: real modems start results on a new line
    uputs(s);                                               // the result text itself
    uputs("\r\n");                                          // end the line so the next prompt starts clean
}                                                           // end result

/* ------------------- lwIP callbacks (network -> us) --------------------- */
/* lwIP is event-driven: we register functions and it calls them when
 * something happens. They run while lwIP holds its lock, so keep them quick. */

static err_t on_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err) {  // called when TCP data arrives
    if (!p) { remote_closed = true; return ERR_OK; }        // NULL pbuf means the far end closed: this is our "NO CARRIER"
    uint32_t freeb = RING_SIZE - (ring_head - ring_tail);   // free space in the ring (unsigned math wraps correctly)
    if (p->tot_len > freeb) return ERR_MEM;                 // no room: tell lwIP to hold the data and retry (flow control)
    uint32_t h = ring_head;                                 // work on a local copy of the write index
    for (struct pbuf *q = p; q; q = q->next) {              // a packet may be a chain of buffer pieces; walk them all
        const uint8_t *d = (const uint8_t *)q->payload;     // pointer to this piece's bytes
        for (u16_t i = 0; i < q->len; i++) ring[h++ & RING_MASK] = d[i];  // copy bytes in, wrapping at the end of the ring
    }                                                       // end chain walk
    ring_head = h;                                          // publish the new write position in one step
    pbuf_free(p);                                           // we copied the data, so release lwIP's buffer
    return ERR_OK;                                          // tell lwIP all is well
}                                                           // end on_recv

static void on_err(void *arg, err_t err) {                  // called if the connection dies (reset, timeout)
    pcb = NULL;                                             // lwIP already freed the pcb, so forget our pointer
    remote_closed = true;                                   // flag it so the main loop reports NO CARRIER
}                                                           // end on_err

static err_t on_connected(void *arg, struct tcp_pcb *tpcb, err_t err) {  // called when the TCP handshake finishes
    connected = (err == ERR_OK);                            // success only if there was no error
    return ERR_OK;                                          // acknowledge the callback
}                                                           // end on_connected

static void dns_found(const char *name, const ip_addr_t *ip, void *arg) {  // called when DNS answers
    if (ip) { resolved = *ip; dns_ok = true; }              // got an address: save it and mark success
    dns_done = true;                                        // either way, the lookup is finished
}                                                           // end dns_found

/* ------------------------- connection control --------------------------- */

static void hangup(void) {                                  // drop the call (like putting the phone back on the hook)
    cyw43_arch_lwip_begin();                                // take the lwIP lock so callbacks can't run mid-change
    if (pcb) {                                              // only if a connection exists
        tcp_arg(pcb, NULL); tcp_recv(pcb, NULL); tcp_err(pcb, NULL);  // unhook our callbacks so nothing fires on a dying pcb
        if (tcp_close(pcb) != ERR_OK) tcp_abort(pcb);       // try a polite close; if that fails, slam it shut
        pcb = NULL;                                         // no connection any more
    }                                                       // end if pcb
    ring_head = ring_tail = 0;                              // throw away any unread incoming data
    cyw43_arch_lwip_end();                                  // release the lock
    ack_pending = 0; txlen = 0;                             // forget pending window updates and unsent bytes
    connected = false; remote_closed = false;               // reset flags for the next call
}                                                           // end hangup

static void tcp_send_raw(const uint8_t *d, size_t n) {      // send bytes straight to the remote host (used for telnet replies)
    cyw43_arch_lwip_begin();                                // lock lwIP
    if (pcb && tcp_sndbuf(pcb) >= n && tcp_write(pcb, d, n, TCP_WRITE_FLAG_COPY) == ERR_OK)  // room available and queue ok?
        tcp_output(pcb);                                    // push the queued data onto the wire now
    cyw43_arch_lwip_end();                                  // unlock
}                                                           // end tcp_send_raw

static void dial(const char *s) {                           // handle "ATD...": the heart of the modem
    while (*s == ' ') s++;                                  // skip spaces after the D
    if ((*s == 'T' || *s == 't' || *s == 'P' || *s == 'p')  // T = tone dial, P = pulse dial (rotary phones)
        && (s[1] == ' ' || (s[1] >= '0' && s[1] <= '9'))) s++;  // only skip it if followed by space/digit, so hostnames starting with t/p survive
    while (*s == ' ') s++;                                  // skip spaces before the number/host

    const char *target = s;                                 // default: treat what was typed as the destination
    for (size_t i = 0; i < sizeof(phonebook) / sizeof(phonebook[0]); i++)  // check each phone book entry
        if (strcmp(s, phonebook[i].number) == 0) { target = phonebook[i].target; break; }  // match? swap in the real host

    char host[96]; uint16_t port = 23;                      // buffer for the hostname; telnet's default port is 23
    strncpy(host, target, sizeof(host) - 1);                // copy the destination so we can edit it
    host[sizeof(host) - 1] = 0;                             // make sure it's null-terminated
    char *colon = strrchr(host, ':');                       // find the last ':' (separates host from port)
    if (colon) { *colon = 0; port = (uint16_t)atoi(colon + 1); }  // split the string and parse the port number
    if (!host[0] || !port) { result("ERROR"); return; }     // nothing usable: modems answer ERROR

    hangup();                                               // make sure any previous call is cleared first
    dns_done = dns_ok = false;                              // reset DNS flags
    cyw43_arch_lwip_begin();                                // lock lwIP for the call
    err_t e = dns_gethostbyname(host, &resolved, dns_found, NULL);  // ask DNS (instant if cached or if it's a plain IP)
    cyw43_arch_lwip_end();                                  // unlock
    if (e == ERR_OK) dns_ok = true;                         // answer was immediate
    else if (e == ERR_INPROGRESS) {                         // answer will come via dns_found later
        for (int i = 0; i < 1000 && !dns_done; i++) sleep_ms(10);  // wait up to ~10 seconds
    }                                                       // end DNS wait
    if (!dns_ok) { result("NO CARRIER"); return; }          // couldn't resolve: report like a failed call

    cyw43_arch_lwip_begin();                                // lock lwIP to create the connection
    pcb = tcp_new_ip_type(IPADDR_TYPE_V4);                  // allocate a new IPv4 TCP connection object
    if (pcb) {                                              // allocation succeeded?
        tcp_arg(pcb, NULL);                                 // no custom argument needed for callbacks
        tcp_recv(pcb, on_recv);                             // register: call on_recv when data arrives
        tcp_err(pcb, on_err);                               // register: call on_err if the connection dies
        e = tcp_connect(pcb, &resolved, port, on_connected);  // start the TCP handshake to host:port
    }                                                       // end if pcb
    cyw43_arch_lwip_end();                                  // unlock
    if (!pcb || e != ERR_OK) { hangup(); result("NO CARRIER"); return; }  // couldn't even start: give up

    for (int i = 0; i < 1500 && !connected && !remote_closed; i++) sleep_ms(10);  // wait up to ~15 s for the handshake
    if (!connected) { hangup(); result("NO CARRIER"); return; }  // handshake failed or timed out

    char msg[32];                                           // buffer for the CONNECT message
    snprintf(msg, sizeof msg, "CONNECT %d", MODEM_BAUD);    // real modems print the speed, e.g. "CONNECT 28800"
    result(msg);                                            // tell the laptop the "call" is up
    data_mode = true;                                       // switch from command mode to passing data through
}                                                           // end dial

/* ---------------------------- command mode ------------------------------ */

static void handle_command(const char *l) {                 // interpret one complete line the laptop typed
    if (strncasecmp(l, "AT", 2) != 0) return;               // every Hayes command starts with AT; ignore anything else
    const char *c = l + 2;                                  // c points at whatever follows "AT"
    if (!*c)                           { result("OK"); return; }  // bare "AT" is the classic "are you there?" test
    if (strncasecmp(c, "D", 1) == 0)   { dial(c + 1); return; }   // ATD... = dial
    if (strncasecmp(c, "H", 1) == 0)   { hangup(); result("OK"); return; }  // ATH = hang up
    if (strncasecmp(c, "Z", 1) == 0)   { echo = true; telnet_filter = true; hangup(); result("OK"); return; }  // ATZ = reset to defaults
    if (strncasecmp(c, "O", 1) == 0) {                      // ATO = go back "online" after a +++ escape
        if (pcb && connected) { data_mode = true; result("CONNECT"); }  // call still up: resume data mode
        else result("NO CARRIER");                          // no call to return to
        return;                                             // done with this command
    }                                                       // end ATO
    if (strncasecmp(c, "NET", 3) == 0) { telnet_filter = (c[3] != '0'); result("OK"); return; }  // ATNET0 raw / ATNET1 telnet (our own extension)
    if (strncasecmp(c, "E0", 2) == 0) echo = false;         // ATE0 = stop echoing typed characters
    else if (strncasecmp(c, "E1", 2) == 0) echo = true;     // ATE1 = echo characters
    result("OK");                                           // everything else (&F, S7=60, X3...) is accepted and ignored
}                                                           // end handle_command

static void command_poll(void) {                            // read typed characters and build up a command line
    static char line[128];                                  // the line being typed (static: survives between calls)
    static int n;                                           // how many characters are in it so far
    while (uart_is_readable(MODEM_UART)) {                  // while the laptop has sent us something
        char ch = uart_getc(MODEM_UART);                    // grab one character
        if (ch == '\n') continue;                           // ignore line feeds; Hayes lines end with carriage return
        if (echo) uart_putc_raw(MODEM_UART, ch);            // echo it back so the user sees what they typed
        if (ch == '\r') {                                   // Enter pressed: the command is complete
            line[n] = 0; n = 0;                             // terminate the string and restart the buffer
            if (line[0]) handle_command(line);              // run it, unless the line was empty
            if (data_mode) return;                          // if that started a call, stop reading commands
        } else if (ch == 8 || ch == 127) {                  // backspace or delete key
            if (n) n--;                                     // drop the last character if there is one
        } else if (n < (int)sizeof(line) - 1) {             // ordinary character and there's room
            line[n++] = ch;                                 // append it to the line
        }                                                   // end character handling
    }                                                       // end while readable
}                                                           // end command_poll

/* ------------------------------ data mode ------------------------------- */

static void queue_tx(uint8_t c) {                           // add one byte to the outgoing staging buffer
    if (txlen < sizeof txbuf) txbuf[txlen++] = c;           // only if there's space
}                                                           // end queue_tx

static void uart_to_tcp(void) {                             // laptop -> internet direction, plus +++ detection
    static uint32_t last_rx_ms, plus_ms;                    // time of last byte seen / time of last '+'
    static int plus_count;                                  // how many '+' in a row we're holding back
    uint32_t now = to_ms_since_boot(get_absolute_time());   // current time in milliseconds

    while (uart_is_readable(MODEM_UART) && txlen < sizeof txbuf - 4) {  // read while data's waiting and we have buffer room
        uint8_t ch = uart_getc(MODEM_UART);                 // get one byte from the laptop
        now = to_ms_since_boot(get_absolute_time());        // refresh the clock
        if (ch == '+' && plus_count < 3                     // a plus, and we haven't already got three...
            && (plus_count > 0 || now - last_rx_ms >= 1000)) {  // ...and either it continues a run, or it follows 1s of silence (guard time)
            plus_count++; plus_ms = now;                    // hold it back: it might be the escape sequence
        } else {                                            // not part of an escape
            while (plus_count) { queue_tx('+'); plus_count--; }  // release any held pluses; they were real data
            queue_tx(ch);                                   // queue this byte for the internet
        }                                                   // end escape check
        last_rx_ms = now;                                   // remember when we last heard from the laptop
    }                                                       // end read loop
    if (plus_count && now - plus_ms >= 1000) {              // pluses held and a full second of silence followed
        if (plus_count == 3) { plus_count = 0; data_mode = false; result("OK"); }  // "+++" confirmed: back to command mode
        else while (plus_count) { queue_tx('+'); plus_count--; }  // only 1-2 pluses: just data, send them
    }                                                       // end guard-time check
    if (txlen && pcb) {                                     // bytes waiting and a connection exists
        bool sent = false;                                  // track whether TCP accepted them
        cyw43_arch_lwip_begin();                            // lock lwIP
        if (pcb && tcp_sndbuf(pcb) >= txlen                 // connection still alive and has send-buffer space...
            && tcp_write(pcb, txbuf, txlen, TCP_WRITE_FLAG_COPY) == ERR_OK) {  // ...and queuing succeeded
            tcp_output(pcb); sent = true;                   // flush to the network and note success
        }                                                   // end write
        cyw43_arch_lwip_end();                              // unlock
        if (sent) txlen = 0;                                // buffer is empty again (if not sent, we retry next loop)
    }                                                       // end flush
}                                                           // end uart_to_tcp

/*
 * Telnet filter. Telnet servers open with negotiation like "IAC DO ECHO"
 * (bytes 255 253 1) asking the client to enable options. A dumb terminal
 * can't do those, so we politely refuse everything:
 *      server says WILL x (251)  -> we reply DONT x (254)
 *      server says DO   x (253)  -> we reply WONT x (252)
 * and we swallow sub-negotiation blocks (IAC SB ... IAC SE).
 */
static void tcp_to_uart(void) {                             // internet -> laptop direction
    static int st;                                          // state machine: 0 normal, 1 got IAC, 2 got verb, 3 in SB block, 4 IAC inside SB
    static uint8_t verb;                                    // remembers which telnet verb (WILL/WONT/DO/DONT) we saw
    while (ring_tail != ring_head && uart_is_writable(MODEM_UART)) {  // while data is waiting and the UART can take a byte
        uint8_t c = ring[ring_tail++ & RING_MASK];          // take the next byte from the ring and advance
        ack_pending++;                                      // count it so we can open the TCP window later
        if (!telnet_filter) { uart_putc_raw(MODEM_UART, c); continue; }  // raw mode: just forward the byte untouched
        switch (st) {                                       // run the telnet state machine
        case 0:                                             // ordinary text state
            if (c == 255) st = 1;                           // 255 = IAC: a command follows
            else uart_putc_raw(MODEM_UART, c);              // plain character: show it on the laptop
            break;                                          // next byte
        case 1:                                             // we just saw IAC
            if (c == 255) { uart_putc_raw(MODEM_UART, 255); st = 0; }  // IAC IAC means a literal 255 data byte
            else if (c >= 251 && c <= 254) { verb = c; st = 2; }       // WILL/WONT/DO/DONT: next byte is the option
            else if (c == 250) st = 3;                      // SB: start of a sub-negotiation block, skip it
            else st = 0;                                    // any other command (NOP etc.): ignore
            break;                                          // next byte
        case 2: {                                           // option byte after a verb
            uint8_t r[3] = {255, 0, c};                     // build a reply: IAC, <verb>, <same option>
            if (verb == 251) { r[1] = 254; tcp_send_raw(r, 3); }       // they WILL -> we say DONT
            else if (verb == 253) { r[1] = 252; tcp_send_raw(r, 3); }  // they DO   -> we say WONT
            st = 0; break; }                                // back to normal (WONT/DONT need no reply)
        case 3: if (c == 255) st = 4; break;                // inside SB: wait for an IAC
        case 4: st = (c == 240) ? 0 : 3; break;             // IAC SE (240) ends the block; otherwise keep skipping
        }                                                   // end switch
    }                                                       // end while data
    if (ack_pending && (ack_pending >= 512 || ring_tail == ring_head)) {  // enough consumed, or buffer drained
        cyw43_arch_lwip_begin();                            // lock lwIP
        if (pcb) tcp_recved(pcb, ack_pending);              // tell TCP we've consumed those bytes so the sender may send more
        cyw43_arch_lwip_end();                              // unlock
        ack_pending = 0;                                    // start counting again
    }                                                       // end window update
}                                                           // end tcp_to_uart

/* --------------------------------- main --------------------------------- */

int main(void) {                                            // program entry point
    stdio_init_all();                                       // set up USB serial so printf() shows on your PC
    uart_init(MODEM_UART, MODEM_BAUD);                      // start the hardware UART at 19200 baud
    gpio_set_function(MODEM_TX_PIN, GPIO_FUNC_UART);        // route GP0 to the UART's transmit line
    gpio_set_function(MODEM_RX_PIN, GPIO_FUNC_UART);        // route GP1 to the UART's receive line
    uart_set_format(MODEM_UART, 8, 1, UART_PARITY_NONE);    // 8N1: 8 data bits, 1 stop bit, no parity (the classic format)
    uart_set_fifo_enabled(MODEM_UART, true);                // use the 32-byte hardware FIFO so we don't drop characters

    if (cyw43_arch_init()) { printf("cyw43 init failed\n"); return 1; }  // power up the Wi-Fi chip; bail if it fails
    cyw43_arch_enable_sta_mode();                           // station mode: join an existing Wi-Fi network as a client
    printf("Connecting to Wi-Fi...\n");                     // debug message over USB
    while (cyw43_arch_wifi_connect_timeout_ms(WIFI_SSID, WIFI_PASSWORD, CYW43_AUTH_WPA2_AES_PSK, 30000)) {  // try for 30 s, repeat on failure
        printf("Wi-Fi connect failed, retrying\n");         // report each failed attempt
    }                                                       // end retry loop
    printf("Wi-Fi connected\n");                            // success
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1);          // turn on the onboard LED as a "Wi-Fi is up" light

    uputs("\r\nPICO MODEM READY\r\n");                      // greeting so you can see the serial link works

    for (;;) {                                              // run forever, like a real modem
        if (data_mode) {                                    // currently "connected"
            if (remote_closed && ring_tail == ring_head) {  // far side hung up and we've shown all its last words
                hangup(); data_mode = false; result("NO CARRIER");  // clean up and announce the dropped call
                continue;                                   // restart the loop in command mode
            }                                               // end hang-up check
            uart_to_tcp();                                  // pass laptop bytes to the internet (and watch for +++)
            tcp_to_uart();                                  // pass internet bytes to the laptop
        } else {                                            // command mode
            command_poll();                                 // listen for AT commands
        }                                                   // end mode check
        sleep_us(200);                                      // tiny pause so we don't spin the CPU at 100%
    }                                                       // end forever loop
}                                                           // end main
