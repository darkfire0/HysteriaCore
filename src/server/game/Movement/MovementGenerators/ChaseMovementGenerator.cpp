/*
 * Copyright (C) 2008-2018 TrinityCore <https://www.trinitycore.org/>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "ChaseMovementGenerator.h"
#include "Creature.h"
#include "G3DPosition.hpp"
#include "MoveSpline.h"
#include "MoveSplineInit.h"
#include "PathGenerator.h"
#include "Unit.h"
#include "Util.h"

static bool IsMutualChase(Unit* owner, Unit* target)
{
    MovementGenerator* gen = target->GetMotionMaster()->top();
    if (!gen || gen->GetMovementGeneratorType() != CHASE_MOTION_TYPE)
        return false;
    return (static_cast<ChaseMovementGenerator const*>(gen)->GetTarget() == owner);
}

static bool PositionOkay(Unit* owner, Unit* target, Optional<float> minDistance, Optional<float> maxDistance, Optional<ChaseAngle> angle)
{
    float const distSq = owner->GetExactDistSq(target);
    if (minDistance && distSq < sqr(*minDistance))
        return false;
    if (maxDistance && distSq > sqr(*maxDistance))
        return false;
    return !angle || angle->IsAngleOkay(target->GetRelativeAngle(owner));
}

ChaseMovementGenerator::ChaseMovementGenerator(Unit* target, Optional<ChaseRange> range, Optional<ChaseAngle> angle) : AbstractFollower(ASSERT_NOTNULL(target)), _range(range), _angle(angle) {}
ChaseMovementGenerator::~ChaseMovementGenerator() = default;

void ChaseMovementGenerator::Initialize(Unit* owner)
{
    owner->AddUnitState(UNIT_STATE_CHASE);
    owner->SetWalk(false);
}

bool ChaseMovementGenerator::Update(Unit* owner, uint32 diff)
{
    // owner might be dead or gone (can we even get nullptr here?)
    if (!owner || !owner->IsAlive())
        return false;

    // our target might have gone away
    Unit* const target = GetTarget();
    if (!target)
        return false;

    // the owner might've selected a different target (feels like we shouldn't check this here...)
    if (owner->GetVictim() != target)
        return false;

    // the owner might be unable to move (rooted or casting), pause movement
    if (owner->HasUnitState(UNIT_STATE_NOT_MOVE) || owner->IsMovementPreventedByCasting())
    {
        owner->StopMoving();
        return true;
    }

    bool  const mutualChase     = IsMutualChase(owner, target);
    float const hitboxSum       = owner->GetCombatReach() + target->GetCombatReach();
    float const minRange        = _range ? _range->MinRange + hitboxSum : CONTACT_DISTANCE;
    float const minTarget       = (_range ? _range->MinTolerance : 0.0f) + hitboxSum;
    float const maxRange        = _range ? _range->MaxRange + hitboxSum : owner->GetMeleeRange(target); // melee range already includes hitboxes
    float const maxTarget       = _range ? _range->MaxTolerance + hitboxSum : CONTACT_DISTANCE + hitboxSum;
    Optional<ChaseAngle> angle  = mutualChase ? Optional<ChaseAngle>() : _angle;

    // if we're already moving, periodically check if we're already in the expected range...
    if (owner->HasUnitState(UNIT_STATE_CHASE_MOVE))
    {
        if (_rangeCheckTimer > diff)
            _rangeCheckTimer -= diff;
        else
        {
            _rangeCheckTimer = RANGE_CHECK_INTERVAL;
            if (PositionOkay(owner, target, _movingTowards ? Optional<float>() : minTarget, _movingTowards ? maxTarget : Optional<float>(), angle))
            {
                _path = nullptr;
                owner->StopMoving();
                owner->SetInFront(target);
                return true;
            }
        }
    }

    // if we're done moving, we want to clean up
    if (owner->HasUnitState(UNIT_STATE_CHASE_MOVE) && owner->movespline->Finalized())
    {
        _path = nullptr;
        owner->ClearUnitState(UNIT_STATE_CHASE_MOVE);
        owner->SetInFront(target);
    }

    bool newpos = false;
    // @emo - trqbva da se opravi tva
    if (_lastTargetPosition.GetPositionX() != target->GetPositionX())
        newpos = true;
    if (_lastTargetPosition.GetPositionY() != target->GetPositionY())
        newpos = true;
    if (_lastTargetPosition.GetPositionZ() != target->GetPositionZ())
        newpos = true;

    // if the target moved, we have to consider whether to adjust
    if (/*_lastTargetPosition != target->GetPosition() ||*/ newpos || mutualChase != _mutualChase)
    {
        _lastTargetPosition = target->GetPosition();
        _mutualChase = mutualChase;
        if (owner->HasUnitState(UNIT_STATE_CHASE_MOVE) || !PositionOkay(owner, target, minRange, maxRange, angle))
        {
            Creature* const cOwner = owner->ToCreature();
            // can we get to the target?
            if (cOwner && !target->isInAccessiblePlaceFor(cOwner))
            {
                cOwner->SetCannotReachTarget(true);
                cOwner->StopMoving();
                _path = nullptr;
                return true;
            }

            // figure out which way we want to move
            bool const moveToward = !owner->IsInDist(target, maxRange);

            // make a new path if we have to...
            if (!_path || moveToward != _movingTowards)
                _path = std::make_unique<PathGenerator>(owner);

            float x, y, z;
            bool shortenPath;
            // if we want to move toward the target and there's no fixed angle...
            if (moveToward && !angle)
            {
                // ...we'll pathfind to the center, then shorten the path
                target->GetPosition(x, y, z);
                shortenPath = true;
            }
            else
            {
                // otherwise, we fall back to nearpoint finding
                target->GetNearPoint(owner, x, y, z, (moveToward ? maxTarget : minTarget) - hitboxSum, 0, angle ? target->ToAbsoluteAngle(angle->RelativeAngle) : target->GetAbsoluteAngle(owner));
                shortenPath = false;
            }

            if (owner->IsHovering())
                owner->UpdateAllowedPositionZ(x, y, z);

            bool success = _path->CalculatePath(x, y, z);
            if (!success || (_path->GetPathType() & PATHFIND_NOPATH))
            {
                if (cOwner)
                    cOwner->SetCannotReachTarget(true);
                owner->StopMoving();
                return true;
            }

            if (shortenPath)
                _path->ShortenPathUntilDist(PositionToVector3(target), maxTarget);

            if (cOwner)
                cOwner->SetCannotReachTarget(false);
            owner->AddUnitState(UNIT_STATE_CHASE_MOVE);

            // Fan Creatures.
            Unit* target = GetTarget();
            Movement::MoveSplineInit init(owner);
            int chaserCount = target->getAttackers().size();
            if (owner->ToCreature() && chaserCount > 1)
            {
                float radius = MIN_MELEE_REACH;

                // Get the Attackers index.
                int chaserIndex = 0;
                std::unordered_set<Unit*>::iterator it;
                for (it = target->getAttackers().begin(); it != target->getAttackers().end(); ++it)
                {
                    if ((*it) == owner)
                        break;
                    chaserIndex++;
                }

                int position = ceil((float) chaserIndex / 2) * (chaserIndex % 2 ? -1 : 1);
                float step = float(M_PI) / chaserCount;
                Position const& pos = target->GetPosition();
                float angle = pos.GetAngle(owner->GetPositionX(), owner->GetPositionY());
                angle += step * position;

                G3D::Vector3 point;
                point.x = pos.m_positionX + radius * cosf(angle);
                point.y = pos.m_positionY + radius * sinf(angle);

                if (owner->IsFlying())
                    point.z = pos.m_positionZ;
                else
                    //point.z = owner->GetMapHeight(point.x, point.y, pos.m_positionZ);
                    point.z = owner->GetMap()->GetHeight(owner->GetPhaseMask(), x, y, z, true);

                _path->popPoint();
                _path->addPoint(point);
            }
            init.MovebyPath(_path->GetPath());
            init.SetWalk(false);
            init.SetFacing(target);

            init.Launch();
        }
    }

    // and then, finally, we're done for the tick
    return true;
}

void ChaseMovementGenerator::Finalize(Unit* owner)
{
    owner->ClearUnitState(UNIT_STATE_CHASE | UNIT_STATE_CHASE_MOVE);
    if (Creature* cOwner = owner->ToCreature())
        cOwner->SetCannotReachTarget(false);
}
