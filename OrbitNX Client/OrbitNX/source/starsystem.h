#ifndef _STARSYSTEM_H
#define _STARSYSTEM_H

#include "context.h"

#define SS_MAX_PLANETS 4
#define SS_TRAIL_LENGTH 40
#define SS_MOON_TRAIL_LENGTH 16
#define SS_STAR_COUNT 140

typedef struct
{
    float x, y;
} SSTrailPoint;

typedef struct
{
    float x, y;
    float vx, vy;
    float mass;
    float visualRadius;
    SDL_Color color;

    SSTrailPoint trail[SS_TRAIL_LENGTH];
    int trailCount;
    int trailHead; // index of the most recently written trail point
    float trailTimer;

    // Kinematic satellite orbiting this planet - deliberately NOT driven by gravity, same lesson
    // learned from the Unity prototype: a moon close enough to look good around its planet is too
    // easily perturbed by the star/neighbours under real N-body gravity to ever guarantee it looks
    // right, so it just walks a perfect circle instead.
    float moonAngle;
    float moonAngularSpeed;
    float moonOrbitRadius;
    float moonVisualRadius;

    // Moon's own (shorter, faster-sampled) trail - separate buffer from the planet's because the
    // moon's world position is derived (planet position + orbit offset), not stored directly.
    SSTrailPoint moonTrail[SS_MOON_TRAIL_LENGTH];
    int moonTrailCount;
    int moonTrailHead;
    float moonTrailTimer;
} SSBody;

typedef struct
{
    float x, y;
    float radius;
    SDL_Color color;
} SSStar;

typedef struct
{
    float centreX, centreY;
    float gravityConstant;

    SSBody star;
    SSBody planets[SS_MAX_PLANETS];
    int planetCount;

    SSStar background[SS_STAR_COUNT];
} StarSystem;

/* Randomly generates a fresh system (planet count, colours, orbits) centred at (centreX, centreY).
 * Call once before the first starsystem_update/draw. */
void starsystem_init(StarSystem *system, float centreX, float centreY);

/* Advances the simulation by dt seconds - real star gravity for the planets (star-only, not
 * planet-to-planet, so nothing can ever perturb a neighbour into a collision), kinematic angle
 * step for each moon. */
void starsystem_update(StarSystem *system, float dt);

/* Draws the starfield, nebula haze, trails, star, planets and moons, in that back-to-front order. */
void starsystem_draw(RenderContext *context, const StarSystem *system);

#endif
