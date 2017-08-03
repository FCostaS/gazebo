/*
 * Copyright (C) 2014 Open Source Robotics Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
*/

#include <list>
#include <queue>
#include <algorithm>

#include "gazebo/common/Assert.hh"

#include "gazebo/physics/World.hh"

#include "gazebo/physics/dart/DARTPhysics.hh"
#include "gazebo/physics/dart/DARTJoint.hh"
#include "gazebo/physics/dart/DARTLink.hh"
#include "gazebo/physics/dart/DARTModel.hh"
#include "gazebo/physics/dart/DARTModelPrivate.hh"

using namespace gazebo;
using namespace physics;

//////////////////////////////////////////////////
DARTModel::DARTModel(BasePtr _parent)
  : Model(_parent), dataPtr(new DARTModelPrivate())
{
}

//////////////////////////////////////////////////
DARTModel::~DARTModel()
{
  delete this->dataPtr;
  this->dataPtr = nullptr;
}

//////////////////////////////////////////////////
void DARTModel::Load(sdf::ElementPtr _sdf)
{
  if (_sdf->HasElement("model"))
  {
    gzerr << "Nested models are not currently supported in DART. ["
      << _sdf->Get<std::string>("name") << "] will not be loaded. "
      << std::endl;
    this->sdf = _sdf;
    return;
  }

  Model::Load(_sdf);
}

//////////////////////////////////////////////////
void DARTModel::Init()
{
  // nested models are not supported for now, issue #1833
  if (this->sdf->HasElement("model"))
    return;

  GZ_ASSERT(this->dataPtr->dtSkeleton, "Skeleton can't be NULL");

  gzdbg << "Initializing DART model " << this->GetName() << "\n";

  //----------------------------------------------------------------
  // Build DART Skeleton from the list of links and joints
  //
  // NOTE: Below code block will be simplified once DART implements
  // SkeletonBuilder which would play a role similiar to Simbody's
  // MultibodyGraphMaker.
  //----------------------------------------------------------------

  LinkPtr worldLink(new DARTLink(
      boost::static_pointer_cast<Model>(shared_from_this())));
  for (auto joint : this->GetJoints())
  {
    if (!joint->GetParent())
    {
      worldLink->AddChildJoint(joint);
    }
  }

  Joint_V loopJoints;
  Link_V allLinks = this->GetLinks();

  std::list<LinkPtr> linksToAdd;
  for (auto link : allLinks)
  {
    linksToAdd.push_back(link);
  }

  std::queue<LinkPtr> linksToProcess;
  linksToProcess.push(worldLink);

  while (!linksToAdd.empty())
  {
    if (linksToProcess.empty())
    {
      // Find a link without parents if possible
      LinkPtr newRoot = linksToAdd.front();
      for (auto link : linksToAdd)
      {
        if (link->GetParentJoints().empty())
        {
          newRoot = link;
          break;
        }
      }

      // Create free joint for new root link and add pair to skeleton
      gzdbg << "Building DART BodyNode for link '" << newRoot->GetName()
            << "' with a free joint.\n";

      if (!DARTModelPrivate::CreateJointAndNodePair(
          this->dataPtr->dtSkeleton, nullptr, nullptr, newRoot))
      {
        gzdbg << "Could not create joint and node.\n";
        break;
      }

      linksToAdd.remove(newRoot);
      linksToProcess.push(newRoot);
    }

    // Add children using BFS
    while (!linksToProcess.empty())
    {
      LinkPtr parentLink = linksToProcess.front();
      linksToProcess.pop();
      for (auto joint : parentLink->GetChildJoints())
      {
        LinkPtr childLink = joint->GetChild();
        if (childLink == nullptr)
        {
          gzerr << "DART does not allow joint without child link. "
                << "Please see issue #914. "
                << "(https://bitbucket.org/osrf/gazebo/issue/914)\n";
          continue;
        }

        auto childLinkItr
            = std::find(linksToAdd.begin(), linksToAdd.end(), childLink);
        if (childLinkItr != linksToAdd.end())
        {
          // Add joint and child link to skeleton
          gzdbg << "Building DART BodyNode for link '" << childLink->GetName()
                << "' and joint '" << joint->GetName() << "'.\n";

          dart::dynamics::BodyNode* dtParentBodyNode =
              this->dataPtr->dtSkeleton->getBodyNode(parentLink->GetName());

          if (!DARTModelPrivate::CreateJointAndNodePair(
              this->dataPtr->dtSkeleton, dtParentBodyNode, joint, childLink))
          {
            gzdbg << "Could not create joint and node.\n";
            // Avoid a potential infinite loop
            linksToAdd.clear();
            break;
          }

          linksToAdd.erase(childLinkItr);
          linksToProcess.push(childLink);
        }
        else
        {
          // Child link has already been added to skeleton
          loopJoints.push_back(joint);
        }
      }
    }
  }

  // Process remaining joints
  for (auto joint : loopJoints)
  {
    gzdbg << "Building DART BodyNode for link '" << joint->GetChild()->GetName()
          << "' and loop joint '" << joint->GetName() << "'.\n";

    dart::dynamics::BodyNode* dtParentBodyNode = nullptr;
    if (joint->GetParent() != nullptr)
    {
      dtParentBodyNode = this->dataPtr->dtSkeleton->getBodyNode(
          joint->GetParent()->GetName());
    }

    if (!DARTModelPrivate::CreateLoopJointAndNodePair(this->DARTWorld(),
        this->dataPtr->dtSkeleton, dtParentBodyNode, joint, joint->GetChild()))
    {
      gzdbg << "Could not create loop joint and node.\n";
      break;
    }
  }

  Model::Init();

  //----------------------------------------------
  // Name
  this->dataPtr->dtSkeleton->setName(this->GetName());

  //----------------------------------------------
  // Static
  this->dataPtr->dtSkeleton->setMobile(!this->IsStatic());

  // Self collision
  // Note: This process should be done after this skeleton is added to the
  //       world.

  // Check whether there exist at least one pair of self collidable links.
  int numSelfCollidableLinks = 0;
  bool hasPairOfSelfCollidableLinks = false;
  for (auto link : allLinks)
  {
    if (link->GetSelfCollide())
    {
      ++numSelfCollidableLinks;
      if (numSelfCollidableLinks >= 2)
      {
        hasPairOfSelfCollidableLinks = true;
        break;
      }
    }
  }

  // If the skeleton has at least two self collidable links, then we set the
  // skeleton as self collidable. If the skeleton is self collidable, then
  // DART regards that all the links in the skeleton is self collidable. So, we
  // disable all the pairs of which both of the links in the pair is not self
  // collidable.
  if (hasPairOfSelfCollidableLinks)
  {
    this->dataPtr->dtSkeleton->enableSelfCollisionCheck();
    this->dataPtr->dtSkeleton->setAdjacentBodyCheck(false);
  }

  // Note: This function should be called after the skeleton is added to the
  //       world.
  this->BackupState();

  // Add the skeleton to the world
  this->DARTWorld()->addSkeleton(this->dataPtr->dtSkeleton);
}


//////////////////////////////////////////////////
void DARTModel::Update()
{
  Model::Update();
}

//////////////////////////////////////////////////
void DARTModel::Fini()
{
  // get a backup of the world, because Model::Fini() (eventually
  // calling Base::Fini()) will reset the world pointer
  dart::simulation::WorldPtr _world = this->DARTWorld();
  // remove all links and joints properly
  Model::Fini();
  // remove the skeleton from the world
  if (_world && this->dataPtr->dtSkeleton)
  {
    _world->removeSkeleton(this->dataPtr->dtSkeleton);
  }
}

//////////////////////////////////////////////////
void DARTModel::BackupState()
{
  GZ_ASSERT(this->dataPtr->dtSkeleton, "Skeleton can't be NULL");
  this->dataPtr->genPositions = this->dataPtr->dtSkeleton->getPositions();
  this->dataPtr->genVelocities = this->dataPtr->dtSkeleton->getVelocities();
}

//////////////////////////////////////////////////
void DARTModel::RestoreState()
{
  GZ_ASSERT(this->dataPtr->dtSkeleton, "Skeleton can't be NULL");

  GZ_ASSERT(static_cast<size_t>(this->dataPtr->genPositions.size()) ==
            this->dataPtr->dtSkeleton->getNumDofs(),
            "Cannot RestoreState, invalid size");
  GZ_ASSERT(static_cast<size_t>(this->dataPtr->genVelocities.size()) ==
            this->dataPtr->dtSkeleton->getNumDofs(),
            "Cannot RestoreState, invalid size");

  this->dataPtr->dtSkeleton->setPositions(this->dataPtr->genPositions);
  this->dataPtr->dtSkeleton->setVelocities(this->dataPtr->genVelocities);
}

//////////////////////////////////////////////////
dart::dynamics::SkeletonPtr DARTModel::DARTSkeleton()
{
  return this->dataPtr->dtSkeleton;
}

//////////////////////////////////////////////////
DARTPhysicsPtr DARTModel::GetDARTPhysics(void) const
{
  if (!this->GetWorld()) return nullptr;
  return boost::dynamic_pointer_cast<DARTPhysics>(
    this->GetWorld()->Physics());
}

//////////////////////////////////////////////////
dart::simulation::WorldPtr DARTModel::DARTWorld(void) const
{
  DARTPhysicsPtr physics = GetDARTPhysics();
  if (!physics) return nullptr;
  return physics->DARTWorld();
}
