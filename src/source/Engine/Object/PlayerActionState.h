#pragma once

#include "Core/Globals/_enum.h"

namespace Engine::Object
{
    // True while a player's action is one of the attack/skill swing animations
    // (PLAYER_ATTACK_FIST .. PLAYER_RIDE_SKILL). The swing animation's playback
    // speed scales with AttackSpeed (see SetAttackSpeed in ZzzCharacter.cpp), so
    // this predicate doubles as the natural attack-cadence gate: hold off the
    // next action until the current swing finishes, and the rate follows attack
    // speed instead of any fixed timer.
    inline bool IsAttackAction(int currentAction)
    {
        return currentAction >= PLAYER_ATTACK_FIST && currentAction <= PLAYER_RIDE_SKILL;
    }

    // True while a player is casting one of the summoner-style skill animations
    // (PLAYER_SKILL_SLEEP .. PLAYER_SKILL_DRAIN_LIFE_FENRIR). SetAttackSpeed scales
    // these with magic speed exactly like the attack swings do, but they sit one
    // past the PLAYER_RIDE_SKILL end of IsAttackAction's range - so code that keys
    // off "this animation is speed-scaled" has to ask for them separately. Note the
    // summon animations right after them are deliberately NOT here: their play speed
    // is a fixed constant.
    inline bool IsSpeedScaledSkillAction(int currentAction)
    {
        return currentAction >= PLAYER_SKILL_SLEEP && currentAction <= PLAYER_SKILL_DRAIN_LIFE_FENRIR;
    }

    // True while a player is sitting or holding a pose (PLAYER_SIT1 ..
    // PLAYER_POSE_FEMALE1). Used to keep these animations from being reset to the
    // stand pose when equipment or class changes.
    inline bool IsSitOrPoseAction(int currentAction)
    {
        return currentAction >= PLAYER_SIT1 && currentAction <= PLAYER_POSE_FEMALE1;
    }
}
