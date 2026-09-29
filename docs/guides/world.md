# Drawing in the world

Shapes in the world, a camera of your own, and a track's leaderboard runs: what a ghost viewer or a line-drawing tool
is built from. All of it needs host 0.14.0.

## Shapes

`Draw::Tube` sweeps a tube along points (x, y, z triples, in the game's centimetres), `Draw::Ball` makes a ball at the
origin to be moved with `Draw::Move`. Each returns an id; `Draw::Show` hides and shows it, `Draw::Remove` removes it,
`Draw::Clear` removes everything the plugin drew. Everything goes when the map changes.

```cpp
array<double> path = {0, 0, 100, 500, 0, 150, 1000, 200, 150};
int line = Draw::Tube(path, 5, 0.2f, 0.8f, 0.4f, false);          // radius, colour, glowing
int see = Draw::Tube(path, 5, 0.2f, 0.8f, 0.4f, false, 0.25f);    // see-through: 25% opaque tinted glass
int ball = Draw::Ball(25, 1, 0.5f, 0.2f, true);
Draw::Move(ball, 500, 0, 150);
```

A glowing shape's colour and brightness change in place with `Draw::Glow`, a see-through one's opacity with
`Draw::Fade`, so things can pulse and fade without being made again. Tinted glass is drawn in front of the stadium's
water like everything else.

## A camera of your own

`Camera::Take` looks through a camera of the plugin's; `Camera::Set` places it (position, pitch, yaw, field of view)
and `Camera::Release` gives the game its view back. One plugin at a time has it. `Camera::Project` says where a point
in the world is on screen, for labels over things.

```cpp
Camera::Take();
Camera::Set(0, -2000, 1500, -30, 90, 90);
float sx, sy;
if (Camera::Project(0, 0, 100, sx, sy))
    label.SetPosition(sx, sy);
```

`Race::HideBall(true)` hides the player's own ball while you show the track.

`Race::BallPosition` says where the ball being played is, in a race or a track editor test run (`Editor::IsTesting`
says when one is on), for drawing its path as it goes. Both need host 0.15.2.

## A track's runs

`Ghosts::Load` downloads the top of the leaderboard on screen (and the player's own run) from Steam; `Ghosts::State`
says how it's going and `Ghosts::Count` how many are here. Each run's position at a moment of it comes from
`Ghosts::Position`, its player's camera from `Ghosts::View`, its checkpoints from `Ghosts::CheckpointOrder` and
`Ghosts::Splits`.

```cpp
Ghosts::Load("", 25);
// every frame
for (int i = 0; i < Ghosts::Count(); i++)
{
    double x, y, z;
    if (Ghosts::Position(i, playTime, x, y, z))
        Draw::Move(balls[i], x, y, z);
}
```

`Ghosts::PlayerBall` makes a run's own ball, in its player's skin and accessory, placed with `Ghosts::PlaceBall`.
For thousands of runs at once, a crowd (`Ghosts::CrowdCreate`, `CrowdMembers`, `CrowdSkins`, `CrowdTrails`,
`CrowdPlace`) draws them together, placed by the host in one call a frame.

`Tracks::` lists the game's tracks and searches the workshop, and opens a track by its key or workshop id.
