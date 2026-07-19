static int example(int ready, int value)
{
    if (ready)
        value += 1;

    if (!ready)
        return false;

    if (!first_call(value, ready) || second_call(value, ready)) {
        value = 2;
    }

    return value;
}
