function start(i: number) {
    switch (i) {
        case 3:
            i = i + 2;
            break;
        case 2:
            i = 0;
        default:
            i = -1;
            break;
    }
    return i;
}
start(3);