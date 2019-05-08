#include "microbe_stage/compound_cloud_system.h"
#include "microbe_stage/simulation_parameters.h"

#include "ThriveGame.h"

#include "engine/player_data.h"
#include "generated/cell_stage_world.h"

#include <Rendering/GeometryHelpers.h>

#include <OgreHardwarePixelBuffer.h>
#include <OgreMaterialManager.h>
#include <OgreMesh2.h>
#include <OgreMeshManager.h>
#include <OgreMeshManager2.h>
#include <OgreRoot.h>
#include <OgreSceneManager.h>
#include <OgreSubMesh2.h>
#include <OgreTechnique.h>
#include <OgreTextureManager.h>

#include <atomic>

using namespace thrive;

constexpr auto OGRE_CLOUD_TEXTURE_BYTES_PER_ELEMENT = 4;

static std::atomic<int> CloudTextureNumber = {0};
static std::atomic<int> CloudMeshNumberCounter = {0};

////////////////////////////////////////////////////////////////////////////////
// CompoundCloudComponent
////////////////////////////////////////////////////////////////////////////////
CompoundCloudComponent::CompoundCloudComponent(CompoundCloudSystem& owner,
    Compound* first,
    Compound* second,
    Compound* third,
    Compound* fourth) :
    Leviathan::Component(TYPE),
    m_textureName("cloud_" + std::to_string(++CloudTextureNumber)),
    m_owner(owner)
{
    if(!first)
        throw std::runtime_error(
            "CompoundCloudComponent needs at least one Compound type");

    const Compound* aux[] = {first, second, third, fourth};

    // Read data
    for(unsigned int i = 0; i < CLOUDS_IN_ONE; i++) {
        if(aux[i]) {
            clouds[i].id = aux[i]->id;
            clouds[i].color = Ogre::Vector4(
                aux[i]->colour.r, aux[i]->colour.g, aux[i]->colour.b, 1.0f);
            clouds[i].viscosity = aux[i]->viscosity;
        }
    }
}

CompoundCloudComponent::~CompoundCloudComponent()
{
    LEVIATHAN_ASSERT(!m_compoundCloudsPlane && !m_sceneNode,
        "CompoundCloudComponent not Released");

    m_owner.cloudReportDestroyed(this);
}

void
    CompoundCloudComponent::Release(Ogre::SceneManager* scene)
{
    // Destroy the plane
    if(m_compoundCloudsPlane) {
        scene->destroyItem(m_compoundCloudsPlane);
        m_compoundCloudsPlane = nullptr;
    }

    // Scenenode
    if(m_sceneNode) {
        scene->destroySceneNode(m_sceneNode);
        m_sceneNode = nullptr;
    }

    if(m_initialized) {

        m_initialized = false;
    }

    // And material
    if(m_planeMaterial) {

        Ogre::MaterialManager::getSingleton().remove(m_planeMaterial);
        m_planeMaterial.reset();
    }

    // Texture
    if(m_texture) {
        Ogre::TextureManager::getSingleton().remove(m_texture);
        m_texture.reset();
    }
}

// ------------------------------------ //
unsigned int
    CompoundCloudComponent::getSlotForCompound(CompoundId compound)
{
    for(unsigned int i = 0; i < CLOUDS_IN_ONE; i++) {
        if(compound == clouds[i].id)
            return i;
    }

    throw std::runtime_error("This cloud doesn't contain the used CompoundId");
}

bool
    CompoundCloudComponent::handlesCompound(CompoundId compound)
{
    for(unsigned int i = 0; i < CLOUDS_IN_ONE; i++) {
        if(compound == clouds[i].id)
            return true;
    }

    return false;
}
// ------------------------------------ //
void
    CompoundCloudComponent::addCloud(CompoundId compound,
        float dens,
        size_t x,
        size_t y)
{
    clouds[getSlotForCompound(compound)].density[x][y] += dens;
}

int
    CompoundCloudComponent::takeCompound(CompoundId compound,
        size_t x,
        size_t y,
        float rate)
{
    unsigned int slot = getSlotForCompound(compound);

    int amountToGive = static_cast<int>(clouds[slot].density[x][y] * rate);
    clouds[slot].density[x][y] -= amountToGive;
    if(clouds[slot].density[x][y] < 1)
        clouds[slot].density[x][y] = 0;

    return amountToGive;
}

int
    CompoundCloudComponent::amountAvailable(CompoundId compound,
        size_t x,
        size_t y,
        float rate)
{
    return clouds[getSlotForCompound(compound)].density[x][y] * rate;
}

void
    CompoundCloudComponent::getCompoundsAt(size_t x,
        size_t y,
        std::vector<std::tuple<CompoundId, float>>& result)
{
    for(const auto& cloudData : clouds) {
        if(cloudData.id != NULL_COMPOUND) {
            const auto amount = cloudData.density[x][y];
            if(amount > 0)
                result.emplace_back(cloudData.id, amount);
        }
    }
}

// ------------------------------------ //
void
    CompoundCloudComponent::recycleToPosition(const Float3& newPosition)
{
    m_position = newPosition;

    // This check is for non-graphical mode
    if(m_sceneNode)
        m_sceneNode->setPosition(
            m_position.X, CLOUD_Y_COORDINATE, m_position.Z);

    for(auto& cloudData : clouds) {
        if(cloudData.id != NULL_COMPOUND) {
            for(size_t x = 0; x < cloudData.density.size(); ++x) {
                for(size_t y = 0; y < cloudData.density[x].size(); ++y) {
                    cloudData.density[x][y] = 0;
                    cloudData.oldDensity[x][y] = 0;
                }
            }
        }
    }
}

////////////////////////////////////////////////////////////////////////////////
// CompoundCloudSystem
////////////////////////////////////////////////////////////////////////////////

void
    CompoundCloudSystem::Init(CellStageWorld& world)
{
    // Use the curl of a Perlin noise field to create a turbulent velocity
    // field.
    // createVelocityField();

    // Skip if no graphics
    if(!Ogre::Root::getSingletonPtr())
        return;

    const auto meshName =
        "CompoundCloudSystem_Plane_" + std::to_string(++CloudMeshNumberCounter);

    // TODO: fix this in the engine to make this method simpler
    // This crashes when used with RenderDoc and doesn't render anything
    // m_planeMesh = Leviathan::GeometryHelpers::CreateXZPlane(
    //     meshName, CLOUD_WIDTH, CLOUD_HEIGHT);

    // Create a background plane on which the fluid clouds will be drawn.
    m_planeMesh = Ogre::MeshManager::getSingleton().createManual(
        meshName, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);

    Ogre::SubMesh* planeSubMesh = m_planeMesh->createSubMesh();

    Ogre::VaoManager* myVaoManager =
        Ogre::Root::getSingleton().getRenderSystem()->getVaoManager();

    Ogre::VertexElement2Vec myVertexElements;
    myVertexElements.push_back(
        Ogre::VertexElement2(Ogre::VET_FLOAT3, Ogre::VES_POSITION));
    myVertexElements.push_back(
        Ogre::VertexElement2(Ogre::VET_FLOAT2, Ogre::VES_TEXTURE_COORDINATES));


    // Simple square plane with 4 vertices & 2 primitive triangles.
    CloudPlaneVertex meshVertices[] = {
        {Ogre::Vector3(-CLOUD_WIDTH, 0, -CLOUD_HEIGHT), Ogre::Vector2(0, 0)},
        {Ogre::Vector3(-CLOUD_WIDTH, 0, CLOUD_HEIGHT), Ogre::Vector2(0, 1)},
        {Ogre::Vector3(CLOUD_WIDTH, 0, CLOUD_HEIGHT), Ogre::Vector2(1, 1)},
        {Ogre::Vector3(CLOUD_WIDTH, 0, -CLOUD_HEIGHT), Ogre::Vector2(1, 0)}};

    Ogre::VertexBufferPacked* myVertexBuffer = myVaoManager->createVertexBuffer(
        myVertexElements, sizeof(meshVertices) / sizeof(CloudPlaneVertex),
        Ogre::BT_IMMUTABLE, meshVertices, false);

    Ogre::VertexBufferPackedVec myVertexBuffers;
    myVertexBuffers.push_back(myVertexBuffer);

    uint16_t myIndices[] = {2, 0, 1, 0, 2, 3};

    Ogre::IndexBufferPacked* myIndexBuffer = myVaoManager->createIndexBuffer(
        Ogre::IndexBufferPacked::IT_16BIT, sizeof(myIndices) / sizeof(uint16_t),
        Ogre::BT_IMMUTABLE, myIndices, false);

    Ogre::VertexArrayObject* myVao = myVaoManager->createVertexArrayObject(
        myVertexBuffers, myIndexBuffer, Ogre::OT_TRIANGLE_LIST);

    planeSubMesh->mVao[Ogre::VpNormal].push_back(myVao);

    // Set the bounds to get frustum culling and LOD to work correctly.
    m_planeMesh->_setBounds(Ogre::Aabb(Ogre::Vector3::ZERO,
        Ogre::Vector3(CLOUD_WIDTH, CLOUD_Y_COORDINATE, CLOUD_HEIGHT)));

    // Need to edit the render queue (for when the item is created)
    world.GetScene()->getRenderQueue()->setRenderQueueMode(
        2, Ogre::RenderQueue::FAST);
}

void
    CompoundCloudSystem::Release(CellStageWorld& world)
{
    // Make sure all of our entities are destroyed //
    // Because their destruction callback unregisters them we have to delete
    // them like this
    while(!m_managedClouds.empty()) {

        world.DestroyEntity(m_managedClouds.begin()->first);
    }

    // Skip if no graphics
    if(!Ogre::Root::getSingletonPtr())
        return;

    // Destroy the shared mesh
    Ogre::MeshManager::getSingleton().remove(m_planeMesh);
}
// ------------------------------------ //
void
    CompoundCloudSystem::registerCloudTypes(CellStageWorld& world,
        const std::vector<Compound>& clouds)
{
    m_cloudTypes = clouds;

    // We do a spawn cycle immediately to make sure that even early code can
    // spawn clouds
    doSpawnCycle(world, Float3(0, 0, 0));
}

bool
    CompoundCloudSystem::addCloud(CompoundId compound,
        float density,
        const Float3& worldPosition)
{
    // Find the target cloud //
    for(auto& cloud : m_managedClouds) {

        const auto& pos = cloud.second->m_position;

        if(cloudContainsPosition(pos, worldPosition)) {
            // Within cloud

            // Skip wrong types
            if(!cloud.second->handlesCompound(compound))
                continue;

            try {
                auto [x, y] = convertWorldToCloudLocal(pos, worldPosition);
                cloud.second->addCloud(compound, density, x, y);

                return true;

            } catch(const Leviathan::InvalidArgument& e) {
                LOG_ERROR("CompoundCloudSystem: can't place cloud because the "
                          "cloud math is "
                          "wrong, exception:");
                e.PrintToLog();
                return false;
            }
        }
    }

    return false;
}

float
    CompoundCloudSystem::takeCompound(CompoundId compound,
        const Float3& worldPosition,
        float rate)
{
    for(auto& cloud : m_managedClouds) {

        const auto& pos = cloud.second->m_position;

        if(cloudContainsPosition(pos, worldPosition)) {
            // Within cloud

            // Skip wrong types
            if(!cloud.second->handlesCompound(compound))
                continue;

            try {
                auto [x, y] = convertWorldToCloudLocal(pos, worldPosition);
                return cloud.second->takeCompound(compound, x, y, rate);

            } catch(const Leviathan::InvalidArgument& e) {
                LOG_ERROR(
                    "CompoundCloudSystem: can't take from cloud because the "
                    "cloud math is "
                    "wrong, exception:");
                e.PrintToLog();
                return false;
            }
        }
    }

    return 0;
}

float
    CompoundCloudSystem::amountAvailable(CompoundId compound,
        const Float3& worldPosition,
        float rate)
{
    for(auto& cloud : m_managedClouds) {

        const auto& pos = cloud.second->m_position;

        if(cloudContainsPosition(pos, worldPosition)) {
            // Within cloud

            // Skip wrong types
            if(!cloud.second->handlesCompound(compound))
                continue;

            try {
                auto [x, y] = convertWorldToCloudLocal(pos, worldPosition);
                return cloud.second->amountAvailable(compound, x, y, rate);

            } catch(const Leviathan::InvalidArgument& e) {
                LOG_ERROR(
                    "CompoundCloudSystem: can't get available compounds "
                    "from cloud because the cloud math is wrong, exception:");
                e.PrintToLog();
                return false;
            }
        }
    }

    return 0;
}

std::vector<std::tuple<CompoundId, float>>
    CompoundCloudSystem::getAllAvailableAt(const Float3& worldPosition)
{
    std::vector<std::tuple<CompoundId, float>> result;

    for(auto& cloud : m_managedClouds) {

        const auto& pos = cloud.second->m_position;

        if(cloudContainsPosition(pos, worldPosition)) {
            // Within cloud

            try {
                auto [x, y] = convertWorldToCloudLocal(pos, worldPosition);
                cloud.second->getCompoundsAt(x, y, result);

            } catch(const Leviathan::InvalidArgument& e) {
                LOG_ERROR(
                    "CompoundCloudSystem: can't get available compounds "
                    "from cloud because the cloud math is wrong, exception:");
                e.PrintToLog();
            }
        }
    }

    return result;
}
// ------------------------------------ //
bool
    CompoundCloudSystem::cloudContainsPosition(const Float3& cloudPosition,
        const Float3& worldPosition)
{
    return !(worldPosition.X < cloudPosition.X - CLOUD_WIDTH ||
             worldPosition.X >= cloudPosition.X + CLOUD_WIDTH ||
             worldPosition.Z < cloudPosition.Z - CLOUD_HEIGHT ||
             worldPosition.Z >= cloudPosition.Z + CLOUD_HEIGHT);
}

bool
    CompoundCloudSystem::cloudContainsPositionWithRadius(
        const Float3& cloudPosition,
        const Float3& worldPosition,
        float radius)
{
    return !(worldPosition.X + radius < cloudPosition.X - CLOUD_WIDTH ||
             worldPosition.X - radius >= cloudPosition.X + CLOUD_WIDTH ||
             worldPosition.Z + radius < cloudPosition.Z - CLOUD_HEIGHT ||
             worldPosition.Z - radius >= cloudPosition.Z + CLOUD_HEIGHT);
}

std::tuple<size_t, size_t>
    CompoundCloudSystem::convertWorldToCloudLocal(const Float3& cloudPosition,
        const Float3& worldPosition)
{
    const auto topLeftRelative =
        Float3(worldPosition.X - (cloudPosition.X - CLOUD_WIDTH), 0,
            worldPosition.Z - (cloudPosition.Z - CLOUD_HEIGHT));

    // Floor is used here because otherwise the last coordinate is wrong
    const auto localX =
        static_cast<size_t>(std::floor(topLeftRelative.X / CLOUD_RESOLUTION));
    const auto localY =
        static_cast<size_t>(std::floor(topLeftRelative.Z / CLOUD_RESOLUTION));

    // Safety check
    if(localX >= CLOUD_SIMULATION_WIDTH || localY >= CLOUD_SIMULATION_HEIGHT)
        throw Leviathan::InvalidArgument("position not within cloud");

    return std::make_tuple(localX, localY);
}

std::tuple<float, float>
    CompoundCloudSystem::convertWorldToCloudLocalForGrab(
        const Float3& cloudPosition,
        const Float3& worldPosition)
{
    const auto topLeftRelative =
        Float3(worldPosition.X - (cloudPosition.X - CLOUD_WIDTH), 0,
            worldPosition.Z - (cloudPosition.Z - CLOUD_HEIGHT));

    // Floor is used here because otherwise the last coordinate is wrong
    // and we don't want our caller to constantly have to call std::floor
    const auto localX = std::floor(topLeftRelative.X / CLOUD_RESOLUTION);
    const auto localY = std::floor(topLeftRelative.Z / CLOUD_RESOLUTION);

    return std::make_tuple(localX, localY);
}

Float3
    CompoundCloudSystem::calculateGridCenterForPlayerPos(const Float3& pos)
{
    // The gaps between the positions is used for calculations here. Otherwise
    // all clouds get moved when the player moves
    return Float3(
        static_cast<int>(std::round(pos.X / CLOUD_X_EXTENT)) * CLOUD_X_EXTENT,
        0,
        static_cast<int>(std::round(pos.Z / CLOUD_Y_EXTENT)) * CLOUD_Y_EXTENT);
}

// ------------------------------------ //
void
    CompoundCloudSystem::Run(CellStageWorld& world)
{
    if(!world.GetNetworkSettings().IsAuthoritative)
        return;

    const int renderTime = Leviathan::TICKSPEED;

    Float3 position = Float3(0, 0, 0);

    // Hybrid client-server version
    if(ThriveGame::Get()) {

        auto playerEntity = ThriveGame::Get()->playerData().activeCreature();

        if(playerEntity == NULL_OBJECT) {

            LOG_WARNING(
                "CompoundCloudSystem: Run: playerData().activeCreature() "
                "is NULL_OBJECT. "
                "Using default position");
        } else {

            try {
                // Get the player's position.
                const Leviathan::Position& posEntity =
                    world.GetComponent_Position(playerEntity);
                position = posEntity.Members._Position;

            } catch(const Leviathan::NotFound&) {
                LOG_WARNING("CompoundCloudSystem: Run: playerEntity(" +
                            std::to_string(playerEntity) + ") has no position");
            }
        }
    }

    doSpawnCycle(world, position);

    for(auto& value : m_managedClouds) {

        if(!value.second->m_initialized) {
            LEVIATHAN_ASSERT(false, "CompoundCloudSystem spawned a cloud that "
                                    "it didn't initialize");
        }

        processCloud(*value.second, renderTime, world.GetFluidSystem());
    }
}

void
    CompoundCloudSystem::setUpCloudLinks(
        std::unordered_map<std::pair<Int2, CompoundId>, CompoundCloudComponent*>& clouds)
{
    for(auto& [index, cloudComponent] : clouds) {
        const Int2 tile = index.first;
        const CompoundId groupId = index.second;

        cloudComponent->m_upperCloud =
            tile.Y == -1 ? nullptr : clouds[{tile + Int2(0, -1), groupId}];
        cloudComponent->m_lowerCloud =
            tile.Y == 1 ? nullptr : clouds[{tile + Int2(0, 1), groupId}];
        cloudComponent->m_leftCloud =
            tile.X == -1 ? nullptr : clouds[{tile + Int2(0, -1), groupId}];
        cloudComponent->m_rightCloud =
            tile.X == 1 ? nullptr : clouds[{tile + Int2(0, 1), groupId}];
    }
}

void
    CompoundCloudSystem::doSpawnCycle(CellStageWorld& world,
        const Float3& playerPos)
{
    // Initial spawning if everything is empty
    if(m_managedClouds.empty()) {
        m_cloudGridCenter = Float3(0, 0, 0);

        const auto requiredCloudPositions{
            calculateGridPositions(m_cloudGridCenter)};

        for(size_t i = 0; i < m_cloudTypes.size(); i += CLOUDS_IN_ONE) {

            // All positions
            for(const auto& pos : requiredCloudPositions) {
                _spawnCloud(world, pos.second, i);
            }
        }

        applyNewCloudPositioning();
    }
    // This rounds up to the nearest multiple of 4,
    // divides that by 4 and multiplies by 9 to get all the clouds we have
    // (if we have 5 compounds that are clouds, we need 18 clouds, if 4 we need
    // 9 etc)
    LEVIATHAN_ASSERT(m_managedClouds.size() ==
                         ((((m_cloudTypes.size() + 4 - 1) / 4 * 4) / 4) * 9),
        "A CompoundCloud entity has mysteriously been destroyed");

    // Calculate what our center should be
    const auto targetCenter = calculateGridCenterForPlayerPos(playerPos);

    // TODO: because we no longer check if the player has moved at least a bit
    // it is possible that this gets triggered very often if the player spins
    // around a cloud edge.

    // This needs an extra variable to track how much the player has moved
    // auto moved = playerPos - m_cloudGridCenter;

    if(m_cloudGridCenter != targetCenter) {

        m_cloudGridCenter = targetCenter;
        applyNewCloudPositioning();
    }
}

void
    CompoundCloudSystem::applyNewCloudPositioning()
{
    std::unordered_map<std::pair<Int2, CompoundId>, CompoundCloudComponent*> clouds;

    // Calculate the new positions
    const auto requiredCloudPositions{
        calculateGridPositions(m_cloudGridCenter)};

    // Reposition clouds according to the origin
    // The max amount of clouds is that all need to be moved
    const size_t MAX_FAR_CLOUDS = m_managedClouds.size();

    // According to spec this check is superfluous, but it makes me feel
    // better
    if(m_tooFarAwayClouds.size() != MAX_FAR_CLOUDS)
        m_tooFarAwayClouds.resize(MAX_FAR_CLOUDS);

    size_t farAwayIndex = 0;

    // All clouds that aren't at one of the requiredCloudPositions needs to
    // be moved. Also only one from each cloud group needs to be at each
    // position
    for(const auto& [id, cloudComponent] : m_managedClouds) {

        const auto pos = cloudComponent->m_position;

        bool matched = false;

        // Check if it is at any of the valid positions
        for(size_t i = 0; i < std::size(requiredCloudPositions); ++i) {

            const auto& requiredPos = requiredCloudPositions[i];

            // An exact check might work but just to be safe slight
            // inaccuracy is allowed here
            if((pos - requiredPos.second).HAddAbs() < Leviathan::EPSILON) {
                clouds[{requiredPos.first, cloudComponent->clouds[0].id}] =
                    cloudComponent;
                matched = true;
                break;
            }
        }

        if(!matched) {

            if(farAwayIndex >= MAX_FAR_CLOUDS) {

                LOG_FATAL("CompoundCloudSystem: Logic error in calculating "
                          "far away clouds that need to move");
                break;
            }

            m_tooFarAwayClouds[farAwayIndex++] = cloudComponent;
        }
    }

    // Move clouds that are too far away
    // We check through each position that should have a cloud and move one
    // where there isn't one. This also needs to take into account the cloud
    // groups

    // Loop through the cloud groups
    for(size_t c = 0; c < m_cloudTypes.size(); c += CLOUDS_IN_ONE) {

        const CompoundId groupType = m_cloudTypes[c].id;

        // Loop for moving clouds to all needed positions for each group
        for(size_t i = 0; i < std::size(requiredCloudPositions); ++i) {

            bool hasCloud = false;

            const auto& requiredPos = requiredCloudPositions[i];

			// Didn't we just do all this?
            for(auto iter = m_managedClouds.begin();
                iter != m_managedClouds.end(); ++iter) {

                const auto pos = iter->second->m_position;
                // An exact check might work but just to be safe slight
                // inaccuracy is allowed here
                if((pos - requiredPos.second).HAddAbs() < Leviathan::EPSILON) {

                    // Check that the group of the cloud is correct
                    if(groupType == iter->second->clouds[0].id) {
                        hasCloud = true;
                        break;
                    }
                }
            }

            if(hasCloud)
                continue;

            bool filled = false;

            // We need to find a cloud from the right group
            for(size_t checkReposition = 0; checkReposition < farAwayIndex;
                ++checkReposition) {

                if(m_tooFarAwayClouds[checkReposition] &&
                    m_tooFarAwayClouds[checkReposition]->clouds[0].id ==
                        groupType) {

                    // Found a candidate
                    m_tooFarAwayClouds[checkReposition]->recycleToPosition(
                        requiredPos.second);
                    clouds[{requiredPos.first,
                        m_tooFarAwayClouds[checkReposition]->clouds[0].id}] =
                        m_tooFarAwayClouds[checkReposition];

                    // Set to null to skip on next scan
                    m_tooFarAwayClouds[checkReposition] = nullptr;

                    filled = true;
                    break;
                }
            }

            if(!filled) {
                LOG_FATAL("CompoundCloudSystem: Logic error in moving far "
                          "clouds, didn't find any to use for needed pos");
                break;
            }
        }
    }

    // TODO: this can be removed once this has been fully confirmed to work fine
    // Errors about clouds that should have been moved but haven't been
    for(size_t checkReposition = 0; checkReposition < farAwayIndex;
        ++checkReposition) {
        if(m_tooFarAwayClouds[checkReposition]) {
            LOG_FATAL(
                "CompoundCloudSystem: Logic error in moving far "
                "clouds, a cloud that should have been moved wasn't moved");
        }
    }

    setUpCloudLinks(clouds);
}

void
    CompoundCloudSystem::_spawnCloud(CellStageWorld& world,
        const Float3& pos,
        size_t startIndex)
{
    auto entity = world.CreateEntity();

    Compound* aux[CLOUDS_IN_ONE];

    for(unsigned int i = 0; i < CLOUDS_IN_ONE; i++) {
        aux[i] = startIndex + i < m_cloudTypes.size() ?
                     &m_cloudTypes[startIndex + i] :
                     nullptr;
    }

    CompoundCloudComponent& cloud = world.Create_CompoundCloudComponent(
        entity, *this, aux[0], aux[1], aux[2], aux[3]);
    m_managedClouds[entity] = &cloud;

    // Set correct position
    // TODO: this should probably be made a constructor parameter
    cloud.m_position = pos;

    initializeCloud(cloud, world.GetScene());
}


void
    CompoundCloudSystem::initializeCloud(CompoundCloudComponent& cloud,
        Ogre::SceneManager* scene)
{
    // All the densities
    for(auto& cloudData : cloud.clouds) {
        if(cloudData.id != NULL_COMPOUND) {
            for(size_t x = 0; x < cloudData.density.size(); ++x) {
                for(size_t y = 0; y < cloudData.density[x].size(); ++y) {
                    cloudData.density[x][y] = 0;
                    cloudData.oldDensity[x][y] = 0;
                }
            }
        }
    }

    cloud.m_initialized = true;

    // Skip if no graphics
    if(!Ogre::Root::getSingletonPtr())
        return;

    // Create where the eventually created plane object will be attached
    cloud.m_sceneNode = scene->getRootSceneNode()->createChildSceneNode();

    // set the position properly
    cloud.m_sceneNode->setPosition(
        cloud.m_position.X, CLOUD_Y_COORDINATE, cloud.m_position.Z);

    // Create a modified material that uses
    cloud.m_planeMaterial = Ogre::MaterialManager::getSingleton().create(
        cloud.m_textureName + "_material", "Generated");

    cloud.m_planeMaterial->setReceiveShadows(false);

    // cloud.m_planeMaterial->createTechnique();
    LEVIATHAN_ASSERT(cloud.m_planeMaterial->getTechnique(0) &&
                         cloud.m_planeMaterial->getTechnique(0)->getPass(0),
        "Ogre material didn't create default technique and pass");
    Ogre::Pass* pass = cloud.m_planeMaterial->getTechnique(0)->getPass(0);

    // Set blendblock
    Ogre::HlmsBlendblock blendblock;
    blendblock.setBlendType(Ogre::SBT_TRANSPARENT_ALPHA);

    // Important for proper blending (not sure,
    // mAlphaToCoverageEnabled seems to be more important as a lot of
    // stuff breaks without it)
    blendblock.mIsTransparent = true;

    pass->setBlendblock(blendblock);
    pass->setVertexProgram("CompoundCloud_VS");
    pass->setFragmentProgram("CompoundCloud_PS");

    // Set colour parameter //
    for(unsigned int i = 0; i < CLOUDS_IN_ONE; i++) {
        pass->getFragmentProgramParameters()->setNamedConstant(
            "cloudColour" + std::to_string(i + 1), cloud.clouds[i].color);
    }

    // The perlin noise texture needs to be tileable. We can't do tricks with
    // the cloud's position

    // Even though we ask for the RGBA format the actual order of pixels when
    // locked for writing is something completely different
    cloud.m_texture = Ogre::TextureManager::getSingleton().createManual(
        cloud.m_textureName, "Generated", Ogre::TEX_TYPE_2D,
        CLOUD_SIMULATION_WIDTH, CLOUD_SIMULATION_HEIGHT, 0, Ogre::PF_BYTE_RGBA,
        Ogre::TU_DYNAMIC_WRITE_ONLY_DISCARDABLE,
        nullptr
        // Gamma correction
        ,
        true);

    LEVIATHAN_ASSERT(Ogre::PixelUtil::getNumElemBytes(Ogre::PF_BYTE_RGBA) ==
                         OGRE_CLOUD_TEXTURE_BYTES_PER_ELEMENT,
        "Pixel format bytes has changed");

    auto pixelBuffer = cloud.m_texture->getBuffer();
    pixelBuffer->lock(Ogre::v1::HardwareBuffer::HBL_DISCARD);
    const Ogre::PixelBox& pixelBox = pixelBuffer->getCurrentLock();

    // Fill with zeroes
    std::memset(
        static_cast<uint8_t*>(pixelBox.data), 0, pixelBuffer->getSizeInBytes());

    // Unlock the pixel buffer
    pixelBuffer->unlock();
    // Make sure it wraps to make the borders also look good
    // TODO: check is this needed. This is absolutely needed for the perlin
    // noise but probably not for the cloud densities. So it is easier to keep
    // this for now
    Ogre::HlmsSamplerblock wrappedBlock;
    wrappedBlock.setAddressingMode(Ogre::TextureAddressingMode::TAM_WRAP);

    auto* densityState = pass->createTextureUnitState();
    densityState->setTexture(cloud.m_texture);
    densityState->setSamplerblock(wrappedBlock);

    Ogre::TexturePtr texturePtr =
        Ogre::TextureManager::getSingleton().load("PerlinNoise.jpg",
            Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    auto* noiseState = pass->createTextureUnitState();
    noiseState->setTexture(texturePtr);

    noiseState->setSamplerblock(wrappedBlock);

    // // Maybe compiling this here is the best place
    // cloud.m_planeMaterial->compile();

    // Needs to create a plane instance on which the material is used on
    cloud.m_compoundCloudsPlane = scene->createItem(m_planeMesh);
    cloud.m_compoundCloudsPlane->setCastShadows(false);

    // This needs to be add to an early render queue
    // But after the background
    cloud.m_compoundCloudsPlane->setRenderQueueGroup(2);

    cloud.m_sceneNode->attachObject(cloud.m_compoundCloudsPlane);

    // This loads the material first time this is called. This needs
    // to be called AFTER the first compound cloud has been created. We are
    // currently initializing one so it is fine
    cloud.m_compoundCloudsPlane->setMaterialName(
        cloud.m_planeMaterial->getName());
}
// ------------------------------------ //
void
    CompoundCloudSystem::cloudReportDestroyed(CompoundCloudComponent* cloud)
{
    for(auto iter = m_managedClouds.begin(); iter != m_managedClouds.end();
        ++iter) {

        if(iter->second == cloud) {
            m_managedClouds.erase(iter);
            return;
        }
    }

    LOG_WARNING("CompoundCloudSystem: non-registered CompoundCloudComponent "
                "reported that it was destroyed");
}
// ------------------------------------ //
void
    CompoundCloudSystem::processCloud(CompoundCloudComponent& cloud,
        int renderTime,
        FluidSystem& fluidSystem)
{
    // Try to slow things down (doesn't seem to work great)
    renderTime /= 10;
    Float2 pos(cloud.m_position.X, cloud.m_position.Z);

    // The diffusion rate seems to have a bigger effect

    // Compound clouds move from area of high concentration to area of low.
    for(unsigned int slot = 0; slot < cloud.clouds.size(); slot++) {
        if(cloud.clouds[slot].id != NULL_COMPOUND) {
            diffuse(0.007f, cloud, slot, renderTime);
            // Move the compound clouds about the velocity field.
            advect(cloud, slot, renderTime, fluidSystem, pos);
        }
    }

    // No graphics check
    if(!cloud.m_texture)
        return;

    // Store the pixel data in a hardware buffer for quick access.
    auto pixelBuffer = cloud.m_texture->getBuffer();

    pixelBuffer->lock(Ogre::v1::HardwareBuffer::HBL_DISCARD);
    const Ogre::PixelBox& pixelBox = pixelBuffer->getCurrentLock();
    auto* pDest = static_cast<uint8_t*>(pixelBox.data);

    const size_t rowBytes =
        pixelBox.rowPitch * OGRE_CLOUD_TEXTURE_BYTES_PER_ELEMENT;

    // Copy the density vector into the buffer.

    // This is probably branch predictor friendly to move each bunch of pixels
    // separately

    // Due to Ogre making the pixelbox lock however it wants the order is
    // actually: Ogre::PF_A8R8G8B8
    if(pixelBox.format != Ogre::PF_A8R8G8B8) {
        LOG_INFO(
            "Pixel format: " + Ogre::PixelUtil::getFormatName(pixelBox.format));
        LEVIATHAN_ASSERT(false,
            "Ogre created texture write lock with unexpected pixel order");
    }

    // Even with that pixel format the actual channel indexes are:
    // so PF_B8G8R8A8 for some reason
    // R - 2
    // G - 1
    // B - 0
    // A - 3

    const unsigned int indices[] = {2, 1, 0, 3};

    if(cloud.clouds[0].id == NULL_COMPOUND)
        LEVIATHAN_ASSERT(false, "cloud with not even the first compound");

    for(unsigned int i = 0; i < CLOUDS_IN_ONE; i++) {
        if(cloud.clouds[i].id != NULL_COMPOUND)
            fillCloudChannel(cloud.clouds[i], indices[i], rowBytes, pDest);
    }

    // Unlock the pixel buffer.
    pixelBuffer->unlock();
}

void
    CompoundCloudSystem::fillCloudChannel(const CloudData& cloudData,
        size_t index,
        size_t rowBytes,
        uint8_t* pDest)
{
    for(size_t i = 0; i < cloudData.density.size(); i++) {
        for(size_t j = 0; j < cloudData.density[i].size(); j++) {

            // This formula smoothens the cloud density so that we get gradients
            // of transparency.
            // TODO: move this to the shaders for better performance (we would
            // need to pass a float instead of a byte).
            int intensity = static_cast<int>(
                255 * 2 * std::atan(0.003f * cloudData.density[i][j]));

            // This is the same clamping code as in the old version
            intensity = std::clamp(intensity, 0, 255);

            pDest[rowBytes * j + (i * OGRE_CLOUD_TEXTURE_BYTES_PER_ELEMENT) +
                  index] = static_cast<uint8_t>(intensity);
        }
    }
}

void
    CompoundCloudSystem::diffuse(float diffRate,
        CompoundCloudComponent& cloudComponent,
        unsigned int slot,
        int dt)
{
    float a = dt * diffRate;
    CloudData& cloudData = cloudComponent.clouds[slot];

    for(int x = 0; x < CLOUD_SIMULATION_WIDTH; x++) {
        for(int y = 0; y < CLOUD_SIMULATION_HEIGHT; y++) {
            float upperDensity = 0.0f;
            if(y > 0)
                upperDensity = cloudData.oldDensity[x][y - 1];
            else if(cloudComponent.m_upperCloud)
                upperDensity = cloudComponent.m_upperCloud->clouds[slot]
                                   .oldDensity[x][CLOUD_SIMULATION_HEIGHT - 1];

            float lowerDensity = 0.0f;
            if(y < CLOUD_SIMULATION_HEIGHT - 1)
                lowerDensity = cloudData.oldDensity[x][y + 1];
            else if(cloudComponent.m_lowerCloud)
                lowerDensity =
                    cloudComponent.m_lowerCloud->clouds[slot].oldDensity[x][0];

            float leftDensity = 0.0f;
            if(x > 0)
                leftDensity = cloudData.oldDensity[x - 1][y];
            else if(cloudComponent.m_leftCloud)
                leftDensity = cloudComponent.m_leftCloud->clouds[slot]
                                  .oldDensity[CLOUD_SIMULATION_WIDTH - 1][y];

            float rightDensity = 0.0f;
            if(x < CLOUD_SIMULATION_WIDTH - 1)
                rightDensity = cloudData.oldDensity[x + 1][y];
            else if(cloudComponent.m_rightCloud)
                rightDensity =
                    cloudComponent.m_rightCloud->clouds[slot].oldDensity[0][y];

            cloudData.oldDensity[x][y] =
                cloudData.density[x][y] * (1 - a) +
                (upperDensity + lowerDensity + leftDensity + rightDensity) * a /
                    4;
        }
    }
}

void
    CompoundCloudSystem::advect(CompoundCloudComponent& cloudComponent,
        unsigned int slot,
        int dt,
        FluidSystem& fluidSystem,
        Float2 pos)
{
    CloudData& cloudData = cloudComponent.clouds[slot];

    for(int x = 0; x < CLOUD_SIMULATION_WIDTH; x++) {
        for(int y = 0; y < CLOUD_SIMULATION_HEIGHT; y++) {
            cloudData.density[x][y] = 0;
        }
    }

    // TODO: this is probably the place to move the compounds on the edges into
    // the next cloud (instead of not handling them here)
    for(size_t x = 0; x < CLOUD_SIMULATION_WIDTH; x++) {
        for(size_t y = 0; y < CLOUD_SIMULATION_HEIGHT; y++) {
            if(cloudData.oldDensity[x][y] > 1) {
                Float2 velocity = fluidSystem.getVelocityAt(
                                      pos + Float2(x, y) * CLOUD_RESOLUTION) /
                                  cloudData.viscosity;

                float dx = x + dt * velocity.X;
                float dy = y + dt * velocity.Y;

                dx = std::clamp(dx, 0.5f, CLOUD_SIMULATION_WIDTH - 1.5f);
                dy = std::clamp(dy, 0.5f, CLOUD_SIMULATION_HEIGHT - 1.5f);

                const int x0 = static_cast<int>(dx);
                const int x1 = x0 + 1;
                const int y0 = static_cast<int>(dy);
                const int y1 = y0 + 1;

                float s1 = dx - x0;
                float s0 = 1.0f - s1;
                float t1 = dy - y0;
                float t0 = 1.0f - t1;

                addCloudDensity(cloudComponent, slot, x0, y0,
                    cloudData.oldDensity[x][y] * s0 * t0);
                addCloudDensity(cloudComponent, slot, x0, y1,
                    cloudData.oldDensity[x][y] * s0 * t1);
                addCloudDensity(cloudComponent, slot, x1, y0,
                    cloudData.oldDensity[x][y] * s1 * t0);
                addCloudDensity(cloudComponent, slot, x1, y1,
                    cloudData.oldDensity[x][y] * s1 * t1);
            }
        }
    }
}

// TODO: come out with a less convoluted method of doing this.
void
    CompoundCloudSystem::addCloudDensity(CompoundCloudComponent& cloudComponent,
        unsigned int slot,
        int x,
        int y,
        float value)
{
    CompoundCloudComponent* xComponent = &cloudComponent;
    if(x < 0) {
        x = CLOUD_SIMULATION_WIDTH - 1;
        xComponent = cloudComponent.m_leftCloud;
    } else if(x >= CLOUD_SIMULATION_WIDTH) {
        x = 0;
        xComponent = cloudComponent.m_rightCloud;
    }

    if(!xComponent)
        return;

    CompoundCloudComponent* yComponent = xComponent;
    if(y < 0) {
        y = CLOUD_SIMULATION_HEIGHT - 1;
        yComponent = xComponent->m_upperCloud;
    } else if(y >= CLOUD_SIMULATION_HEIGHT) {
        y = 0;
        yComponent = xComponent->m_lowerCloud;
    }

    if(!yComponent)
        return;

    yComponent->clouds[slot].density[x][y] += value;
}
