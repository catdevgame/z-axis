#pragma once

// A level of any of the three games as a model in the converters' terms
// (collection/levels/model.h), as the tooling's zge_level_gltf.py exports
// one:
// - objects: every placed object, a mesh per object and a node per
//   instance at its world transform. An object whose drawn part is not its
//   collision part gives the drawn part tagged #deco and the collision part
//   #hidden; faces that do not collide in a collidable part go in a #deco
//   mesh of their own; instances neither shown nor collidable are left out.
// - the sky: the instance with the sky bit is a #sky node.
// - materials: one per texture, surface kind and sidedness, named
//   "<texture>#<tags>" (#qp, #spine, #kicker=P with #launch=P, #slope=G,
//   #wall, #metal, #wood, #2sided, #2sided=collide, #layer=<state>).
// - grind edges: line meshes #grind=ledge|lip|spine|vpole|hpole.
// - spawns: the scene's Spawn points, PlayerOne and PlayerTwo as
//   #spawn=PlayerOne and #spawn=PlayerTwo, the respawns (AI's "Respawn",
//   DM2's "Player", BMX XXX's other spawns but paintball's) as
//   #spawn=respawn; spawns only a game's scripts or modes name are left out.
// - props: an object of the DynamicsObject type with its ZDYN definition is
//   a #dynamics node, its mesh's extras the definition, the object's class
//   data and the instance's tags (a Woodward trash can is a prop in the
//   other two games' maps too, with that game's sounds).
// - name hashes: an object whose word is not its name field's hash (the
//   field cut the name: the word is the whole name's) keeps the word in its
//   mesh's extras.
// - cameras: the "CameraPos" points named "idle" in their index order as
//   #camera nodes with #fov=N (the first tag byte), at the eye (DM2's 5 m
//   above its point).
// Axes: the games' +y is down; the model's (x, y, z) is the games'
// (x, -y, -z).

#include "collection/levels/model.h"

#include <map>
#include <string>

namespace zx::app::levels
{
    // A level's files by extension ("GEO", "INS", "DAT", "TXR", "ZIT").
    using LevelFiles = std::map<std::string, Bytes>;

    // The level's model. Throws LevelError on a file that does not read.
    Model exportLevel(Engine engine, const LevelFiles &files);
}
