\ Custom ASCII logo for ISCRA bootloader
\ Display ISCRA logo at boot time

: logo ( x y -- )
    \ Save position
    2dup

    \ Display the ASCII art logo
    ." " cr
    ."  ___ ____   ____ ____      _    " cr
    ." |_ _/ ___| / ___|  _ \    / \   " cr
    ."  | |\___ \| |   | |_) |  / _ \  " cr
    ."  | | ___) | |___|  _ <  / ___ \ " cr
    ." |___|____/ \____|_| \_\/_/   \_\" cr
    ." " cr

    \ Clean up stack
    2drop
;
