"use strict";
// BigInt Table oracle: one literal per line, printed in order so the VM output
// is the expected value of each table entry. Sizes cover the sign-byte cases of
// the encoding (127 fits one byte, 128 and 255 need a second byte for the sign),
// a value used twice (one table entry), 64-bit edges, and values of 30 and 61
// decimal digits.
print(0n);
print(1n);
print(127n);
print(128n);
print(255n);
print(256n);
print(65535n);
print(9223372036854775807n);
print(9223372036854775808n);
print(18446744073709551616n);
print(123456789012345678901234567890n);
print(123456789012345678901234567890n);
print(0xffffffffffffffffffffffffn);
print(1606938044258990275541962092341162602522202993782792835301376n);
