#pragma once

// The level's collision set-up made on the host (collision_setup_host.cpp).

namespace zge::native
{
    struct HostCollisionPart;
}

namespace aggressive
{
    // The words setupCollisionPart writes at load, written into a host part's
    // tag tables: AI's level-setup step for zge/native (setLevelSetupStep).
    void applyLevelSetupOnHost(zge::native::HostCollisionPart &part);
}
