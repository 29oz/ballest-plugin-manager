// Regression fixture: loops forever every tenth frame. The host cuts each one short at the 20 ms budget and stops the
// plugin once it has run out of time more often than it lets off in a minute.
int frames = 0;

void Update(float dt)
{
    if (++frames % 10 == 0)
        while (true) {}
}
