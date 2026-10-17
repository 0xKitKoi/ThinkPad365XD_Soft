' ============================================================
'  XFER.BAS - file transfer client for the Pico Wi-Fi modem
' ============================================================
'  Talks to xfer_server.py on your modern PC through the Pico.
'  Protocol: a text command line, then XMODEM-CRC for the data.
'
'  XMODEM (Ward Christensen, 1977) sends 128-byte blocks. Each block
'  is: SOH, block#, 255-block#, 128 data bytes, 2 CRC bytes.
'  The receiver answers ACK (good) or NAK (resend). The receiver
'  starts things by sending 'C' (= "I want the CRC version").
'
'  Run:  QBASIC /RUN XFER.BAS
' ============================================================

DECLARE SUB PutByte (b%)
DECLARE FUNCTION GetByte% (tmo!)
DECLARE FUNCTION ReadLine$ (tmo!)
DECLARE SUB Purge ()
DECLARE SUB SendLine (s$)
DECLARE FUNCTION WaitFor% (target$, tmo!)
DECLARE FUNCTION CalcCrc& (d$)
DECLARE FUNCTION FileSize& (f$)
DECLARE FUNCTION ReadPacket% (expected%, pkt$)
DECLARE SUB XRecv (fname$, fsize&, ok%)
DECLARE SUB XSend (fname$, ok%)
DECLARE SUB Connect ()

DIM SHARED crctab&(255)       ' lookup table so the CRC runs fast
DIM SHARED TimedOut%          ' set by ReadLine$ when nothing arrived

' --- build the CRC-16 table (polynomial &H1021) once ---
FOR i% = 0 TO 255
    c& = i% * 256&
    FOR j% = 1 TO 8
        IF (c& AND &H8000&) <> 0 THEN
            c& = ((c& * 2&) XOR &H1021&) AND &HFFFF&
        ELSE
            c& = (c& * 2&) AND &HFFFF&
        END IF
    NEXT j%
    crctab&(i%) = c&
NEXT i%

CLS
PRINT "XFER - Pico modem file transfer"
PRINT

' Open COM1. CS0,DS0,CD0 = ignore handshake lines (no loopback wiring needed).
' RB2048 = 2 KB receive buffer. If you get "Bad file name", delete options
' from the right end of this string, one at a time, until it opens.
OPEN "COM1:19200,N,8,1,CS0,DS0,CD0,RB2048" FOR RANDOM AS #1
WIDTH #1, 255                 ' never insert line breaks into outgoing data

Connect

DO
    PRINT
    INPUT "(L)ist, (G)et, (P)ut, (Q)uit: ", cmd$
    cmd$ = UCASE$(LEFT$(cmd$, 1))

    SELECT CASE cmd$
    CASE "L"
        SendLine "LIST"
        DO
            ln$ = ReadLine$(10)
            IF TimedOut% OR ln$ = "." THEN EXIT DO
            PRINT ln$
        LOOP

    CASE "G"
        INPUT "Remote file: ", rf$
        INPUT "Save as (DOS 8.3 name): ", lf$
        IF FileSize&(lf$) >= 0 THEN
            PRINT "That file already exists here. Pick another name."
        ELSE
            SendLine "GET " + rf$
            ln$ = ReadLine$(10)
            IF LEFT$(ln$, 3) = "OK " THEN
                fsize& = VAL(MID$(ln$, 4))
                XRecv lf$, fsize&, ok%
                IF ok% THEN
                    PRINT "Got"; fsize&; "bytes."
                ELSE
                    PRINT "Transfer failed. Quit and restart to resync."
                    Purge
                END IF
            ELSE
                PRINT ln$
            END IF
        END IF

    CASE "P"
        INPUT "Local file: ", lf$
        INPUT "Name on server: ", rf$
        sz& = FileSize&(lf$)
        IF sz& < 0 THEN
            PRINT "No such file."
        ELSE
            SendLine "PUT " + rf$ + " " + LTRIM$(STR$(sz&))
            ln$ = ReadLine$(10)
            IF ln$ = "OK" THEN
                XSend lf$, ok%
                IF ok% THEN
                    PRINT ReadLine$(10)
                ELSE
                    PRINT "Transfer failed. Quit and restart to resync."
                    Purge
                END IF
            ELSE
                PRINT ln$
            END IF
        END IF

    CASE "Q"
        SendLine "QUIT"
        EXIT DO
    END SELECT
LOOP

CLOSE #1
PRINT "Bye."
END

' ------------------------------------------------------------
' Dial the server through the Pico modem
' ------------------------------------------------------------
SUB Connect
    INPUT "Server address (ip:port): ", addr$
    SendLine "ATNET0"                     ' raw mode: no telnet filtering of our binary data
    IF NOT WaitFor%("OK", 3) THEN
        PRINT "Modem did not answer. Check cable and baud rate."
        END
    END IF
    SendLine "ATDT " + addr$
    PRINT "Dialing..."
    IF NOT WaitFor%("CONNECT", 30) THEN
        PRINT "No connection."
        END
    END IF
    ln$ = ReadLine$(10)
    IF LEFT$(ln$, 4) <> "XFER" THEN
        PRINT "Unexpected greeting: "; ln$
        END
    END IF
    PRINT "Connected."
END SUB

' ------------------------------------------------------------
' Low-level serial helpers
' ------------------------------------------------------------
SUB PutByte (b%)
    PRINT #1, CHR$(b%);                   ' trailing ; stops QBasic adding CR/LF
END SUB

SUB SendLine (s$)
    PRINT #1, s$ + CHR$(13);              ' Hayes and our server both end lines with CR
END SUB

' Get one byte, or -1 if nothing arrives within tmo! seconds
FUNCTION GetByte% (tmo!)
    t! = TIMER
    DO
        IF LOC(1) > 0 THEN
            GetByte% = ASC(INPUT$(1, #1))
            EXIT FUNCTION
        END IF
        IF TIMER < t! THEN t! = TIMER     ' TIMER restarts at midnight
    LOOP WHILE TIMER - t! < tmo!
    GetByte% = -1
END FUNCTION

' Read one text line (CR/LF stripped). Sets TimedOut% on timeout.
FUNCTION ReadLine$ (tmo!)
    s$ = ""
    TimedOut% = 0
    DO
        c% = GetByte%(tmo!)
        IF c% = -1 THEN
            TimedOut% = -1
            EXIT DO
        END IF
        IF c% = 10 THEN
            IF LEN(s$) > 0 THEN EXIT DO
        ELSEIF c% <> 13 THEN
            s$ = s$ + CHR$(c%)
        END IF
    LOOP
    ReadLine$ = s$
END FUNCTION

' Wait until target$ shows up in the incoming text. Returns -1 (true) or 0.
FUNCTION WaitFor% (target$, tmo!)
    buf$ = ""
    t! = TIMER
    DO
        c% = GetByte%(.2)
        IF c% <> -1 THEN
            buf$ = buf$ + CHR$(c%)
            IF LEN(buf$) > 200 THEN buf$ = RIGHT$(buf$, 100)
            IF INSTR(buf$, target$) > 0 THEN
                WaitFor% = -1
                EXIT FUNCTION
            END IF
            IF INSTR(buf$, "NO CARRIER") > 0 OR INSTR(buf$, "ERROR") > 0 THEN
                WaitFor% = 0
                EXIT FUNCTION
            END IF
        END IF
        IF TIMER < t! THEN t! = TIMER
    LOOP WHILE TIMER - t! < tmo!
    WaitFor% = 0
END FUNCTION

' Discard incoming bytes until the line has been quiet for 0.3 s
SUB Purge
    t! = TIMER
    DO
        IF LOC(1) > 0 THEN
            junk$ = INPUT$(LOC(1), #1)
            t! = TIMER
        END IF
        IF TIMER < t! THEN t! = TIMER
    LOOP WHILE TIMER - t! < .3
END SUB

' CRC-16 of a string, using the table built at startup
FUNCTION CalcCrc& (d$)
    crc& = 0
    FOR i% = 1 TO LEN(d$)
        crc& = ((crc& * 256&) AND &HFFFF&) XOR crctab&((crc& \ 256&) XOR ASC(MID$(d$, i%, 1)))
    NEXT i%
    CalcCrc& = crc&
END FUNCTION

' Size of a local file in bytes, or -1 if it does not exist
FUNCTION FileSize& (f$)
    failed% = 0
    ON ERROR GOTO NoFile
    OPEN f$ FOR INPUT AS #2
    IF failed% = 0 THEN
        FileSize& = LOF(2)
        CLOSE #2
    ELSE
        FileSize& = -1
    END IF
    ON ERROR GOTO 0
    EXIT FUNCTION
NoFile:
    failed% = -1
    RESUME NEXT
END FUNCTION

' ------------------------------------------------------------
' XMODEM-CRC
' ------------------------------------------------------------

' Read the 132 bytes after an SOH. Returns 1 = new good block,
' 2 = duplicate of the last block, 0 = damaged or out of order.
FUNCTION ReadPacket% (expected%, pkt$)
    ReadPacket% = 0
    t! = TIMER
    DO WHILE LOC(1) < 132
        IF TIMER < t! THEN t! = TIMER
        IF TIMER - t! > 3 THEN EXIT FUNCTION
    LOOP
    d$ = INPUT$(132, #1)
    blk% = ASC(LEFT$(d$, 1))
    cpl% = ASC(MID$(d$, 2, 1))
    pkt$ = MID$(d$, 3, 128)
    crc& = ASC(MID$(d$, 131, 1)) * 256& + ASC(MID$(d$, 132, 1))
    IF blk% + cpl% <> 255 THEN EXIT FUNCTION
    IF CalcCrc&(pkt$) <> crc& THEN EXIT FUNCTION
    IF blk% = expected% THEN
        ReadPacket% = 1
    ELSEIF blk% = ((expected% + 255) AND 255) THEN
        ReadPacket% = 2
    END IF
END FUNCTION

' Receive a file from the server into fname$ (fsize& = exact length)
SUB XRecv (fname$, fsize&, ok%)
    ok% = 0
    OPEN fname$ FOR BINARY AS #2
    expected% = 1
    written& = 0
    errs% = 0
    c% = -1
    FOR i% = 1 TO 15                      ' poke the sender with 'C' every 3 s
        PutByte 67
        c% = GetByte%(3)
        IF c% <> -1 THEN EXIT FOR
    NEXT i%
    IF c% = -1 THEN
        PRINT "Server never started sending."
        CLOSE #2
        KILL fname$
        EXIT SUB
    END IF

    DO
        IF c% = 4 THEN                    ' EOT: that was the last block
            PutByte 6
            ok% = -1
            EXIT DO
        ELSEIF c% = 24 THEN               ' CAN: sender gave up
            PRINT "Server cancelled."
            EXIT DO
        ELSEIF c% = 1 THEN                ' SOH: a block follows
            r% = ReadPacket%(expected%, pkt$)
            IF r% = 1 THEN
                n& = fsize& - written&    ' last block is padded, so trim it
                IF n& > 128 THEN n& = 128
                IF n& > 0 THEN
                    out$ = LEFT$(pkt$, n&)
                    PUT #2, , out$
                    written& = written& + n&
                END IF
                expected% = (expected% + 1) AND 255
                errs% = 0
                PutByte 6
                PRINT ".";
            ELSEIF r% = 2 THEN
                PutByte 6                 ' duplicate: just ACK again
            ELSE
                Purge
                PutByte 21                ' NAK: please resend
                errs% = errs% + 1
            END IF
        ELSE                              ' noise or timeout
            Purge
            PutByte 21
            errs% = errs% + 1
        END IF
        IF errs% > 10 THEN
            PutByte 24
            PutByte 24
            PRINT "Too many errors."
            EXIT DO
        END IF
        c% = GetByte%(10)
    LOOP
    PRINT
    CLOSE #2
    IF ok% = 0 THEN KILL fname$           ' don't leave a broken file behind
END SUB

' Send local file fname$ to the server
SUB XSend (fname$, ok%)
    ok% = 0
    OPEN fname$ FOR BINARY AS #2
    remaining& = LOF(2)
    c% = 0
    t! = TIMER
    DO                                    ' wait up to 90 s for the server's 'C'
        c% = GetByte%(5)
        IF c% = 67 THEN EXIT DO
        IF TIMER < t! THEN t! = TIMER
    LOOP WHILE TIMER - t! < 90
    IF c% <> 67 THEN
        PRINT "Server never started."
        CLOSE #2
        EXIT SUB
    END IF

    blk% = 1
    DO WHILE remaining& > 0
        n% = 128
        IF remaining& < 128 THEN n% = remaining&
        tmp$ = SPACE$(n%)
        GET #2, , tmp$
        d$ = STRING$(128, 26)             ' pad a short last block with Ctrl-Z
        MID$(d$, 1, n%) = tmp$
        crc& = CalcCrc&(d$)
        pk$ = CHR$(1) + CHR$(blk%) + CHR$(255 - blk%) + d$ + CHR$(crc& \ 256) + CHR$(crc& AND 255)
        tries% = 0
        DO
            PRINT #1, pk$;
            r% = GetByte%(10)
            IF r% = 67 THEN r% = GetByte%(10)   ' ignore a stray 'C'
            IF r% = 6 THEN EXIT DO
            tries% = tries% + 1
        LOOP WHILE tries% < 10
        IF r% <> 6 THEN
            PRINT "Block failed too many times."
            CLOSE #2
            EXIT SUB
        END IF
        blk% = (blk% + 1) AND 255         ' block numbers wrap 255 -> 0
        remaining& = remaining& - n%
        PRINT ".";
    LOOP
    PRINT

    FOR i% = 1 TO 10                      ' EOT, retried until ACKed
        PutByte 4
        IF GetByte%(5) = 6 THEN
            ok% = -1
            EXIT FOR
        END IF
    NEXT i%
    CLOSE #2
END SUB
