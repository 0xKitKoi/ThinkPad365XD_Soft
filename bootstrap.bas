DECLARE FUNCTION Rd% (t!)
DECLARE FUNCTION Ln$ (t!)
OPEN "COM1:19200,N,8,1,CS0,DS0,CD0,RB2048" FOR RANDOM AS #1
WIDTH #1, 255
INPUT "Server ip:port: ", a$
INPUT "Remote file: ", f$
INPUT "Save as: ", s$
PRINT #1, "ATE0" + CHR$(13);
t! = TIMER: DO WHILE TIMER - t! < 1: LOOP
IF LOC(1) > 0 THEN x$ = INPUT$(LOC(1), #1)
PRINT #1, "ATNET0" + CHR$(13);
x$ = Ln$(3)
PRINT #1, "ATDT " + a$ + CHR$(13);
PRINT "Dialing..."
DO
    x$ = Ln$(30)
    IF x$ = "" OR LEFT$(x$, 2) = "NO" THEN PRINT "No connect": END
LOOP UNTIL LEFT$(x$, 7) = "CONNECT"
x$ = Ln$(10)
PRINT #1, "GET " + f$ + CHR$(13);
x$ = Ln$(10)
IF LEFT$(x$, 3) <> "OK " THEN PRINT x$: END
n& = VAL(MID$(x$, 4))
OPEN s$ FOR BINARY AS #2
e% = 1: w& = 0
FOR i% = 1 TO 15
    PRINT #1, "C";
    c% = Rd%(3)
    IF c% <> -1 THEN EXIT FOR
NEXT i%
DO
    IF c% = 4 THEN
        PRINT #1, CHR$(6);
        EXIT DO
    END IF
    IF c% <> 1 THEN PRINT "Bad start byte"; c%: EXIT DO
    t! = TIMER
    DO WHILE LOC(1) < 132 AND TIMER - t! < 3: LOOP
    IF LOC(1) < 132 THEN PRINT "Timeout": EXIT DO
    d$ = INPUT$(132, #1)
    b% = ASC(LEFT$(d$, 1))
    IF b% = e% THEN
        k& = n& - w&
        IF k& > 128 THEN k& = 128
        IF k& > 0 THEN
            o$ = MID$(d$, 3, k&)
            PUT #2, , o$
            w& = w& + k&
        END IF
        e% = (e% + 1) AND 255
        PRINT ".";
    ELSEIF b% <> ((e% + 255) AND 255) THEN
        PRINT "Out of sync": EXIT DO
    END IF
    PRINT #1, CHR$(6);
    c% = Rd%(10)
LOOP
CLOSE
PRINT : PRINT "Wrote"; w&; "of"; n&; "bytes"
END

FUNCTION Rd% (t!)
    s! = TIMER
    DO
        IF LOC(1) > 0 THEN Rd% = ASC(INPUT$(1, #1)): EXIT FUNCTION
    LOOP WHILE TIMER - s! < t!
    Rd% = -1
END FUNCTION

FUNCTION Ln$ (t!)
    s$ = ""
    DO
        c% = Rd%(t!)
        IF c% = -1 THEN EXIT DO
        IF c% = 10 THEN
            IF s$ <> "" THEN EXIT DO
        ELSEIF c% <> 13 THEN
            s$ = s$ + CHR$(c%)
        END IF
    LOOP
    Ln$ = s$
END FUNCTION
