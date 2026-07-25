#include "starsystem.h"
#include "renderer.h"

#include <switch.h>
#include <stdlib.h>
#include <math.h>
#include <SDL2/SDL2_gfxPrimitives.h>

#define SS_TWO_PI 6.28318530718f

#define SS_STAR_MASS 20000.0f
#define SS_PLANET_MASS 60.0f
#define SS_INNER_ORBIT_RADIUS 90.0f
#define SS_ORBIT_RADIUS_STEP 65.0f
#define SS_ORBIT_JITTER 8.0f
#define SS_GRAVITY_CONSTANT 60.0f
#define SS_TRAIL_SAMPLE_SECONDS 0.08f
#define SS_MOON_TRAIL_SAMPLE_SECONDS 0.025f
#define SS_STAR_VISUAL_RADIUS 32.0f

static const int kAllowedPlanetCounts[] = {2, 4};
static const SDL_Color kRedColour = {230, 60, 30, 255};
static const SDL_Color kBlueColour = {60, 130, 255, 255};
static const SDL_Color kStarColour = {255, 250, 235, 255};

static float ssRandRange(float minValue, float maxValue)
{
    float t = (float)rand() / (float)RAND_MAX;
    return minValue + t * (maxValue - minValue);
}

static void initPlanet(SSBody *planet, float centreX, float centreY, float orbitRadius, SDL_Color colour)
{
    float angle = ssRandRange(0.0f, SS_TWO_PI);
    // Circular-orbit speed: v = sqrt(G * starMass / r) - same formula the Unity prototype used
    // (OrbitMechanicsUtility), just evaluated directly here instead of through an ECS system.
    float speed = sqrtf(SS_GRAVITY_CONSTANT * SS_STAR_MASS / orbitRadius);

    planet->x = centreX + cosf(angle) * orbitRadius;
    planet->y = centreY + sinf(angle) * orbitRadius;
    planet->vx = -sinf(angle) * speed;
    planet->vy = cosf(angle) * speed;
    planet->mass = SS_PLANET_MASS;
    planet->visualRadius = ssRandRange(13.0f, 18.0f);
    planet->color = colour;
    planet->trailCount = 0;
    planet->trailHead = 0;
    planet->trailTimer = 0.0f;

    planet->moonAngle = ssRandRange(0.0f, SS_TWO_PI);
    planet->moonAngularSpeed = ssRandRange(1.8f, 3.0f);
    planet->moonOrbitRadius = planet->visualRadius + 10.0f;
    planet->moonVisualRadius = 5.5f;
    planet->moonTrailCount = 0;
    planet->moonTrailHead = 0;
    planet->moonTrailTimer = 0.0f;
}

void starsystem_init(StarSystem *system, float centreX, float centreY)
{
    srand((unsigned int)svcGetSystemTick());

    system->centreX = centreX;
    system->centreY = centreY;
    system->gravityConstant = SS_GRAVITY_CONSTANT;

    system->star.x = centreX;
    system->star.y = centreY;
    system->star.vx = 0.0f;
    system->star.vy = 0.0f;
    system->star.mass = SS_STAR_MASS;
    system->star.visualRadius = SS_STAR_VISUAL_RADIUS;
    system->star.color = kStarColour;
    system->star.trailCount = 0;

    int allowedCount = sizeof(kAllowedPlanetCounts) / sizeof(kAllowedPlanetCounts[0]);
    system->planetCount = kAllowedPlanetCounts[rand() % allowedCount];

    int blueCount = system->planetCount / 2; // odd totals (only 2/4 here, so always even, but safe)

    for (int i = 0; i < system->planetCount; i++)
    {
        float radius = SS_INNER_ORBIT_RADIUS + i * SS_ORBIT_RADIUS_STEP + ssRandRange(-SS_ORBIT_JITTER, SS_ORBIT_JITTER);
        SDL_Color colour = (i < blueCount) ? kBlueColour : kRedColour;
        initPlanet(&system->planets[i], centreX, centreY, radius, colour);
    }

    // Fisher-Yates shuffle of just the colour assignment (positions/orbits above are already fixed
    // by index) so colour families don't end up grouped on one side of the system.
    for (int i = system->planetCount - 1; i > 0; i--)
    {
        int j = rand() % (i + 1);
        SDL_Color tmp = system->planets[i].color;
        system->planets[i].color = system->planets[j].color;
        system->planets[j].color = tmp;
    }

    for (int i = 0; i < SS_STAR_COUNT; i++)
    {
        system->background[i].x = ssRandRange(0.0f, (float)RESX);
        system->background[i].y = ssRandRange(0.0f, (float)RESY);
        system->background[i].radius = ssRandRange(0.6f, 1.8f);
        Uint8 shade = (Uint8)ssRandRange(140.0f, 255.0f);
        SDL_Color colour = {shade, shade, shade, 255};
        system->background[i].color = colour;
    }
}

static void recordTrailPointGeneric(SSTrailPoint *trail, int trailLength, int *trailHead, int *trailCount, float x, float y)
{
    *trailHead = (*trailHead + 1) % trailLength;
    trail[*trailHead].x = x;
    trail[*trailHead].y = y;
    if (*trailCount < trailLength)
    {
        (*trailCount)++;
    }
}

static void updatePlanet(SSBody *planet, float centreX, float centreY, float starMass, float gravityConstant, float dt)
{
    // Gravity only pulls toward the star, never toward a neighbouring planet - this is what
    // guarantees planets can never perturb each other into a collision, the exact failure mode
    // that took the longest to track down in the Unity prototype.
    float dx = centreX - planet->x;
    float dy = centreY - planet->y;
    float distSq = dx * dx + dy * dy;
    float dist = sqrtf(distSq);
    if (dist > 0.001f)
    {
        float accel = gravityConstant * starMass / distSq;
        planet->vx += (dx / dist) * accel * dt;
        planet->vy += (dy / dist) * accel * dt;
    }

    planet->x += planet->vx * dt;
    planet->y += planet->vy * dt;

    planet->moonAngle += planet->moonAngularSpeed * dt;

    planet->trailTimer += dt;
    if (planet->trailTimer >= SS_TRAIL_SAMPLE_SECONDS)
    {
        planet->trailTimer = 0.0f;
        recordTrailPointGeneric(planet->trail, SS_TRAIL_LENGTH, &planet->trailHead, &planet->trailCount, planet->x, planet->y);
    }

    planet->moonTrailTimer += dt;
    if (planet->moonTrailTimer >= SS_MOON_TRAIL_SAMPLE_SECONDS)
    {
        planet->moonTrailTimer = 0.0f;
        float moonX = planet->x + cosf(planet->moonAngle) * planet->moonOrbitRadius;
        float moonY = planet->y + sinf(planet->moonAngle) * planet->moonOrbitRadius;
        recordTrailPointGeneric(planet->moonTrail, SS_MOON_TRAIL_LENGTH, &planet->moonTrailHead, &planet->moonTrailCount, moonX, moonY);
    }
}

void starsystem_update(StarSystem *system, float dt)
{
    for (int i = 0; i < system->planetCount; i++)
    {
        updatePlanet(&system->planets[i], system->centreX, system->centreY, system->star.mass, system->gravityConstant, dt);
    }
}

// Shared by both the planet's own trail and its moon's (smaller/faster-sampled) one - width and
// alpha both taper from maxWidth/maxAlpha right behind the body down to a hairline at the tail.
static void drawTrailGeneric(RenderContext *context, const SSTrailPoint *trail, int trailLength,
                              int trailHead, int trailCount, SDL_Color colour, float maxWidth, float maxAlpha)
{
    if (trailCount < 2) return;

    for (int i = 1; i < trailCount; i++)
    {
        int newIndex = (trailHead - i + 1 + trailLength * 2) % trailLength;
        int oldIndex = (trailHead - i + trailLength * 2) % trailLength;

        float t = 1.0f - ((float)i / (float)trailCount);
        SDL_Color segColour = colour;
        segColour.a = (Uint8)(t * maxAlpha);

        Uint8 width = (Uint8)(1.0f + t * (maxWidth - 1.0f));

        thickLineRGBA(context->renderer,
                      (Sint16)trail[newIndex].x, (Sint16)trail[newIndex].y,
                      (Sint16)trail[oldIndex].x, (Sint16)trail[oldIndex].y,
                      width, segColour.r, segColour.g, segColour.b, segColour.a);
    }
}

void starsystem_draw(RenderContext *context, const StarSystem *system)
{
    for (int i = 0; i < SS_STAR_COUNT; i++)
    {
        const SSStar *s = &system->background[i];
        SDL_SetRenderDrawColor(context->renderer, s->color.r, s->color.g, s->color.b, s->color.a);
        SDL_Rect dot = {(int)s->x, (int)s->y, (int)ceilf(s->radius), (int)ceilf(s->radius)};
        SDL_RenderFillRect(context->renderer, &dot);
    }

    for (int i = 0; i < system->planetCount; i++)
    {
        const SSBody *planet = &system->planets[i];
        drawTrailGeneric(context, planet->trail, SS_TRAIL_LENGTH, planet->trailHead, planet->trailCount,
                          planet->color, planet->visualRadius * 2.0f, 200.0f);
        drawTrailGeneric(context, planet->moonTrail, SS_MOON_TRAIL_LENGTH, planet->moonTrailHead, planet->moonTrailCount,
                          planet->color, planet->moonVisualRadius * 2.0f, 200.0f);
    }

    fillCircle(context, system->star.x, system->star.y, system->star.visualRadius, system->star.color);

    for (int i = 0; i < system->planetCount; i++)
    {
        const SSBody *planet = &system->planets[i];
        fillCircle(context, planet->x, planet->y, planet->visualRadius, planet->color);

        float moonX = planet->x + cosf(planet->moonAngle) * planet->moonOrbitRadius;
        float moonY = planet->y + sinf(planet->moonAngle) * planet->moonOrbitRadius;
        fillCircle(context, moonX, moonY, planet->moonVisualRadius, planet->color);
    }
}
