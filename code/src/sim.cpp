#include <madrona/mw_gpu_entry.hpp>

#include "sim.hpp"
#include "level_gen.hpp"

#include <algorithm>

using namespace madrona;
using namespace madrona::math;
using namespace madrona::phys;

namespace RenderingSystem = madrona::render::RenderingSystem;

namespace madEscape {

// Register all the ECS components and archetypes that will be
// used in the simulation
void Sim::registerTypes(ECSRegistry &registry, const Config &cfg)
{
    base::registerTypes(registry);
    phys::PhysicsSystem::registerTypes(registry);

    RenderingSystem::registerTypes(registry, cfg.renderBridge);

    registry.registerComponent<Action>();
    registry.registerComponent<SelfObservation>();
    registry.registerComponent<GrabState>();
    registry.registerComponent<Lidar>();
    registry.registerComponent<Active>();
    registry.registerComponent<EntityType>();
    registry.registerComponent<Reward>();
    registry.registerComponent<RewardHelper>();
    registry.registerComponent<Done>();
    registry.registerComponent<StepsRemaining>();
    registry.registerComponent<MacGuffinState>();

    registry.registerSingleton<WorldReset>();

    registry.registerArchetype<Agent>();
    registry.registerArchetype<PhysicsEntity>();
    registry.registerArchetype<EpisodeTracker>();
    registry.registerArchetype<MacGuffin>();
    registry.registerArchetype<Goal>();


    registry.exportSingleton<WorldReset>(
        (uint32_t)ExportID::Reset);
    registry.exportColumn<Agent, Action>(
        (uint32_t)ExportID::Action);
    registry.exportColumn<Agent, SelfObservation>(
        (uint32_t)ExportID::SelfObservation);
    registry.exportColumn<Agent, Lidar>(
        (uint32_t)ExportID::Lidar);
    registry.exportColumn<Agent, Active>(
        (uint32_t)ExportID::Active);
    registry.exportColumn<EpisodeTracker, Reward>(
        (uint32_t)ExportID::Reward);
    registry.exportColumn<EpisodeTracker, Done>(
        (uint32_t)ExportID::Done);
    registry.exportColumn<EpisodeTracker, StepsRemaining>(
        (uint32_t)ExportID::StepsRemaining);
}

static inline void cleanupWorld(Engine &ctx)
{
    // destroy non-persistent entities
    for (CountT i = 0; i < consts::maxCubes; i++) {
        if (ctx.data().cubes[i] != Entity::none()) {
            ctx.destroyRenderableEntity(ctx.data().cubes[i]);
        }
    }
    for (CountT i = 0; i < consts::maxBarriers; i++) {
        if (ctx.data().barriers[i] != Entity::none()) {
            ctx.destroyRenderableEntity(ctx.data().barriers[i]);
        }
    }
}

static inline void initWorld(Engine &ctx)
{
    phys::PhysicsSystem::reset(ctx);

    // Assign a new episode ID
    ctx.data().rng = RNG(rand::split_i(ctx.data().initRandKey,
        ctx.data().curWorldEpisode++, (uint32_t)ctx.worldID().idx));

    // Defined in src/level_gen.hpp / src/level_gen.cpp
    generateWorld(ctx);
}

// This system runs each frame and checks if the current episode is complete
// or if code external to the application has forced a reset by writing to the
// WorldReset singleton.
//
// If a reset is needed, cleanup the existing world and generate a new one.
inline void resetSystem(Engine &ctx, WorldReset &reset)
{
    int32_t should_reset = reset.reset;
    if (ctx.data().autoReset) {
        Entity episodeTracker = ctx.data().episodeTracker;
        Done done = ctx.get<Done>(episodeTracker);
        if (done.v) {
            should_reset = 1;
        }
    }

    if (should_reset != 0) {
        reset.reset = 0;

        cleanupWorld(ctx);
        initWorld(ctx);
    }
}

static inline int32_t uniformInt(Engine &ctx, int32_t min, int32_t max)
{
    return static_cast<int32_t>(ctx.data().rng.sampleUniform() * (max + 1 - min) + min);
}

// Function to be filled
inline Action generateRandomAction(Engine &ctx, Action currAction) {
    /*
        - always move forward
        - with probability 1%, grab
        - every 20 steps, start turning left, straight, or right
    */

    // 1. Always move forward
    // Assuming higher bucket index means more amount.
    // Use max bucket for "always move forward".
    currAction.moveAmount = consts::numMoveAmountBuckets - 1; 
    // Assuming bucket 0 for moveAngle corresponds to agent's local forward.
    currAction.moveAngle = 0; 

    // 2. With probability 1%, grab
    if (ctx.data().rng.sampleUniform() < 0.01f) { // sampleUniform() is [0, 1)
        currAction.grab = 1;
    } else {
        currAction.grab = 0;
    }

    // 3. Every 20 steps, start turning left, straight, or right.
    //    On other steps, go straight.
    //    Access the 't' member of StepsRemaining.
    int32_t steps_remaining_val = ctx.get<StepsRemaining>(ctx.data().episodeTracker).t;
   
    if (steps_remaining_val % 20 == 0) {
        // Randomly choose to turn left, go straight, or turn right.
        // uniformInt(ctx, min, max) is inclusive of min and max.
        int32_t turn_choice = uniformInt(ctx, 0, 2); // 0: left, 1: straight, 2: right

        if (turn_choice == 0) { // Turn Left
            // Assuming bucket 0 is maximum left turn.
            currAction.rotate = 0; 
        } else if (turn_choice == 1) { // Go Straight
            // Middle bucket for turning means no rotation.
            currAction.rotate = consts::numTurnBuckets / 2; 
        } else { // Turn Right (turn_choice == 2)
            // Assuming highest bucket index is maximum right turn.
            currAction.rotate = consts::numTurnBuckets - 1; 
        }
    }
   
    return currAction; // Return the modified action, or just rely on modification by reference
}

// Translates discrete actions from the Action component to forces
// used by the physics simulation.
inline void movementSystem(Engine &ctx,
                           Action &action, 
                           Rotation &rot, 
                           ExternalForce &external_force,
                           ExternalTorque &external_torque)
{
    // action = generateRandomAction(ctx, action);

    constexpr float move_max = consts::agentMoveSpeed;
    constexpr float turn_max = consts::agentTurnSpeed; // these scale with mass apparently

    Quat cur_rot = rot;

    float move_amount = action.moveAmount *
        (move_max / (consts::numMoveAmountBuckets - 1));

    constexpr float move_angle_per_bucket =
        2.f * math::pi / float(consts::numMoveAngleBuckets);

    float move_angle = float(action.moveAngle) * move_angle_per_bucket;

    float f_x = move_amount * sinf(move_angle);
    float f_y = move_amount * cosf(move_angle);

    constexpr float turn_delta_per_bucket = 
        turn_max / (consts::numTurnBuckets / 2);
    float t_z =
        turn_delta_per_bucket * (action.rotate - consts::numTurnBuckets / 2);

    external_force = cur_rot.rotateVec({ f_x, f_y, 0 });
    external_torque = Vector3 { 0, 0, t_z };
}

// Implements the grab action by casting a short ray in front of the agent
// and creating a joint constraint if a grabbable entity is hit.
inline void grabSystem(Engine &ctx,
                       Entity e,
                       Position pos,
                       Rotation rot,
                       Action action,
                       GrabState &grab)
{
    // lowkey don't edit these values unless you know what you're doing
    const float GRAB_RANGE = consts::agentRadius + 1.0f;
    const float GRAB_POINT_FORWARD_OFFSET = 1.25f;
    const float GRAB_POINT_UP_OFFSET = 0.5f;

    if (action.grab == 0) {
        return;
    }

    // if a grab is currently in progress, triggering the grab action
    // just releases the object
    if (grab.constraintEntity != Entity::none()) {
        ctx.destroyEntity(grab.constraintEntity);
        grab.constraintEntity = Entity::none();
        
        return;
    } 

    // Get the per-world BVH singleton component
    auto &bvh = ctx.singleton<broadphase::BVH>();
    float hit_t;
    Vector3 hit_normal;

    Vector3 ray_o = pos + GRAB_POINT_UP_OFFSET * math::up;
    Vector3 ray_d = rot.rotateVec(math::fwd);

    Entity grab_entity =
        bvh.traceRay(ray_o, ray_d, &hit_t, &hit_normal, GRAB_RANGE);

    if (grab_entity == Entity::none()) {
        return;
    }

    auto response_type = ctx.get<ResponseType>(grab_entity);
    if (response_type != ResponseType::Dynamic) {
        return;
    }

    auto entity_type = ctx.get<EntityType>(grab_entity);
    if (entity_type == EntityType::Agent) {
        return;
    }

    Vector3 other_pos = ctx.get<Position>(grab_entity);
    Quat other_rot = ctx.get<Rotation>(grab_entity);

    Vector3 r1 = GRAB_POINT_FORWARD_OFFSET * math::fwd + GRAB_POINT_UP_OFFSET * math::up;

    Vector3 hit_pos = ray_o + ray_d * hit_t;
    Vector3 r2 =
        other_rot.inv().rotateVec(hit_pos - other_pos);

    Quat attach1 = { 1, 0, 0, 0 };
    Quat attach2 = (other_rot.inv() * rot).normalize();

    float separation = hit_t - GRAB_POINT_FORWARD_OFFSET;

    grab.constraintEntity = PhysicsSystem::makeFixedJoint(ctx,
        e, grab_entity, attach1, attach2, r1, r2, separation);
}

// Make the agents easier to control by zeroing out their velocity
// after each step.
inline void agentZeroVelSystem(Engine &,
                               Velocity &vel,
                               Action &)
{
    vel.linear.x = 0;
    vel.linear.y = 0;
    vel.linear.z = fminf(vel.linear.z, 0);

    vel.angular = Vector3::zero();
}

static inline float distObs(float v)
{
    return v / consts::worldLength;
}

static inline float globalPosObs(float v)
{
    return v / consts::worldLength;
}

static inline float angleObs(float v)
{
    return v / math::pi;
}

static inline float encodeType(EntityType type)
{
    return (float)type / (float)EntityType::NumTypes;
}

static inline float computeZAngle(Quat q)
{
    float siny_cosp = 2.f * (q.w * q.z + q.x * q.y);
    float cosy_cosp = 1.f - 2.f * (q.y * q.y + q.z * q.z);
    return atan2f(siny_cosp, cosy_cosp);
}

// Translate xy delta to polar observations for learning.
static inline PolarObservation xyToPolar(Vector3 v)
{
    Vector2 xy{v.x, v.y};

    float r = xy.length();

    // Note that this is angle off y-forward
    float theta = atan2f(xy.x, xy.y);

    return PolarObservation{
        .r = distObs(r),
        .theta = angleObs(theta),
    };
}

// This system packages all the egocentric observations together 
// for the policy inputs.
inline void collectObservationsSystem(Engine &ctx,
                                      Position pos,
                                      Rotation rot,
                                      const GrabState &grab,
                                      SelfObservation &self_obs)
{
    self_obs.globalX = globalPosObs(pos.x);
    self_obs.globalY = globalPosObs(pos.y);
    self_obs.globalZ = globalPosObs(pos.z);
    self_obs.theta = angleObs(computeZAngle(rot));
    self_obs.isGrabbing = grab.constraintEntity != Entity::none() ?
        1.f : 0.f;

    // Get positions of important objects
    Vector3 macguffin_pos = ctx.get<Position>(ctx.data().macguffin);
    Vector3 goal_pos = ctx.get<Position>(ctx.data().goal);

    // Calculate vectors to important objects in world space
    Vector3 to_macguffin = macguffin_pos - pos;
    Vector3 to_goal = goal_pos - pos;

    // Convert to ant's local coordinate system
    Quat to_view = rot.inv();
    Vector3 local_to_macguffin = to_view.rotateVec(to_macguffin);
    Vector3 local_to_goal = to_view.rotateVec(to_goal);

    // Convert to polar coordinates for observations
    PolarObservation polar_to_macguffin = xyToPolar(local_to_macguffin);
    PolarObservation polar_to_goal = xyToPolar(local_to_goal);

    // Store polar observations
    self_obs.polarToMacguffinR = polar_to_macguffin.r;
    self_obs.polarToMacguffinTheta = polar_to_macguffin.theta;
    self_obs.polarToGoalR = polar_to_goal.r;
    self_obs.polarToGoalTheta = polar_to_goal.theta;
}

// Launches consts::numLidarSamples per agent.
// This system is specially optimized in the GPU version:
// a warp of threads is dispatched for each invocation of the system
// and each thread in the warp traces one lidar ray for the agent.
inline void lidarSystem(Engine &ctx,
                        Entity e,
                        Lidar &lidar)
{
    Vector3 pos = ctx.get<Position>(e);
    Quat rot = ctx.get<Rotation>(e);
    auto &bvh = ctx.singleton<broadphase::BVH>();

    Vector3 agent_fwd = rot.rotateVec(math::fwd);
    Vector3 right = rot.rotateVec(math::right);

    auto traceRay = [&](int32_t idx) {
        float theta = 2.f * math::pi * (
            float(idx) / float(consts::numLidarSamples)) + math::pi / 2.f;
        float x = cosf(theta);
        float y = sinf(theta);

        Vector3 ray_dir = (x * right + y * agent_fwd).normalize();

        float hit_t;
        Vector3 hit_normal;
        Entity hit_entity =
            bvh.traceRay(pos + 0.5f * math::up, ray_dir, &hit_t,
                         &hit_normal, 200.f);

        if (hit_entity == Entity::none()) {
            lidar.samples[idx] = {
                .depth = 0.f,
                .encodedType = encodeType(EntityType::None),
            };
        } else {
            EntityType entity_type = ctx.get<EntityType>(hit_entity);

            lidar.samples[idx] = {
                .depth = distObs(hit_t),
                .encodedType = encodeType(entity_type),
            };
        }
    };


    // MADRONA_GPU_MODE guards GPU specific logic
#ifdef MADRONA_GPU_MODE
    // Can use standard cuda variables like threadIdx for 
    // warp level programming
    int32_t idx = threadIdx.x % 32;

    if (idx < consts::numLidarSamples) {
        traceRay(idx);
    }
#else
    for (CountT i = 0; i < consts::numLidarSamples; i++) {
        traceRay(i);
    }
#endif
}

// Computes reward for each agent and keeps track of the max distance achieved
// so far through the challenge. Continuous reward is provided for any new
// distance achieved.
inline void rewardSystem(Engine &ctx,
                         Reward &out_reward,
                         RewardHelper &reward_helper,
                         Done &done,
                         StepsRemaining &steps_remaining
                        )
{
    // // basically the simplest possible reward
    // float r = 0.0f;
    // for (int32_t i = 0; i < consts::maxAgents; i++) {
    //     r += ctx.get<Position>(ctx.data().agents[i]).x;
    // }
    // out_reward.v = r / consts::maxAgents;

    // average -dist to macguffin
    float r = 0.0f;
    for (int32_t i = 0; i < consts::maxAgents; i++) {
        r += (ctx.get<Position>(ctx.data().agents[i]) - ctx.get<Position>(ctx.data().macguffin)).length();
    }
    out_reward.v = -1.0 * r / consts::maxAgents;


    // // Get positions of macguffin and goal
    // Vector3 macguffin_pos = ctx.get<Position>(ctx.data().macguffin);
    // Vector3 goal_pos = ctx.get<Position>(ctx.data().goal);

    // // Calculate 2D distance (ignore Z axis)
    // Vector2 macguffin_pos_2d(macguffin_pos.x, macguffin_pos.y);
    // Vector2 goal_pos_2d(goal_pos.x, goal_pos.y);
    // float dist = (goal_pos_2d - macguffin_pos_2d).length();

    // // First time initialization
    // if (reward_helper.prev_dist < 0)
    // {
    //     reward_helper.prev_dist = dist;
    //     reward_helper.starting_dist = dist;
    // }

    // // Step reward based on distance reduction
    // // Dividing by starting dist ensures that on success, sum of distance rewards is ~1 (times rewardScale)
    // float step_reward = consts::distanceRewardScale * (reward_helper.prev_dist - dist) / reward_helper.starting_dist;

    // // Goal reward if close enough
    // float goal_reward = 0.0f;
    // // Calculate goal's XY bounds
    // float goal_min_x = goal_pos.x - consts::goalSize / 2.0f;
    // float goal_max_x = goal_pos.x + consts::goalSize / 2.0f;
    // float goal_min_y = goal_pos.y - consts::goalSize / 2.0f;
    // float goal_max_y = goal_pos.y + consts::goalSize / 2.0f;
    // // Check if macguffin's XY center is within goal's XY bounds
    // bool within_bounds = (macguffin_pos.x >= goal_min_x && macguffin_pos.x <= goal_max_x &&
    //                         macguffin_pos.y >= goal_min_y && macguffin_pos.y <= goal_max_y);
    // if (within_bounds)
    // {
    //     goal_reward = consts::goalReward;
    //     done.v = 1; // Episode complete on goal achievement
    // }

    // // Existential penalty per timestep
    // float exist_penalty = consts::existentialPenalty;

    // // Total reward for this step
    // out_reward.v = step_reward + goal_reward + exist_penalty;

    // // Store current distance for next step
    // reward_helper.prev_dist = dist;
}

// Keep track of the number of steps remaining in the episode and
// notify training that an episode has completed by
// setting done = 1 on the final step of the episode
inline void stepTrackerSystem(Engine &,
                              StepsRemaining &steps_remaining,
                              Done &done)
{
    int32_t num_remaining = --steps_remaining.t;
    if (num_remaining == consts::episodeLen - 1) {
        done.v = 0;
    } else if (num_remaining == 0) {
        done.v = 1;
    }

}

// Helper function for sorting nodes in the taskgraph.
// Sorting is only supported / required on the GPU backend,
// since the CPU backend currently keeps separate tables for each world.
// This will likely change in the future with sorting required for both
// environments
#ifdef MADRONA_GPU_MODE
template <typename ArchetypeT>
TaskGraph::NodeID queueSortByWorld(TaskGraph::Builder &builder,
                                   Span<const TaskGraph::NodeID> deps)
{
    auto sort_sys =
        builder.addToGraph<SortArchetypeNode<ArchetypeT, WorldID>>(
            deps);
    auto post_sort_reset_tmp =
        builder.addToGraph<ResetTmpAllocNode>({sort_sys});

    return post_sort_reset_tmp;
}
#endif

// Build the task graph
void Sim::setupTasks(TaskGraphManager &taskgraph_mgr, const Config &cfg)
{
    TaskGraphBuilder &builder = taskgraph_mgr.init(TaskGraphID::Step);

    // Turn policy actions into movement
    auto move_sys = builder.addToGraph<ParallelForNode<Engine,
        movementSystem,
            Action,
            Rotation,
            ExternalForce,
            ExternalTorque
        >>({});

    // Build BVH for broadphase / raycasting
    auto broadphase_setup_sys = phys::PhysicsSystem::setupBroadphaseTasks(
        builder, {move_sys});

    // Grab action, post BVH build to allow raycasting
    auto grab_sys = builder.addToGraph<ParallelForNode<Engine,
        grabSystem,
            Entity,
            Position,
            Rotation,
            Action,
            GrabState
        >>({broadphase_setup_sys});

    // Physics collision detection and solver
    auto substep_sys = phys::PhysicsSystem::setupPhysicsStepTasks(builder,
        {grab_sys}, consts::numPhysicsSubsteps);

    // Improve controllability of agents by setting their velocity to 0
    // after physics is done.
    auto agent_zero_vel = builder.addToGraph<ParallelForNode<Engine,
        agentZeroVelSystem, Velocity, Action>>(
            {substep_sys});

    // Finalize physics subsystem work
    auto phys_done = phys::PhysicsSystem::setupCleanupTasks(
        builder, {agent_zero_vel});

    // Compute initial reward now that physics has updated the world state
    auto reward_sys = builder.addToGraph<ParallelForNode<Engine,
         rewardSystem,
            Reward,
            RewardHelper,
            Done,
            StepsRemaining
        >>({phys_done});

    // Check if the episode is over
    auto done_sys = builder.addToGraph<ParallelForNode<Engine,
        stepTrackerSystem,
            StepsRemaining,
            Done
        >>({reward_sys});

    // Conditionally reset the world if the episode is over
    auto reset_sys = builder.addToGraph<ParallelForNode<Engine,
        resetSystem,
            WorldReset
        >>({done_sys});

    auto clear_tmp = builder.addToGraph<ResetTmpAllocNode>({reset_sys});
    (void)clear_tmp;

#ifdef MADRONA_GPU_MODE
    // RecycleEntitiesNode is required on the GPU backend in order to reclaim
    // deleted entity IDs.
    auto recycle_sys = builder.addToGraph<RecycleEntitiesNode>({reset_sys});
    (void)recycle_sys;
#endif

    // This second BVH build is a limitation of the current taskgraph API.
    // It's only necessary if the world was reset, but we don't have a way
    // to conditionally queue taskgraph nodes yet.
    auto post_reset_broadphase = phys::PhysicsSystem::setupBroadphaseTasks(
        builder, {reset_sys});

    // Finally, collect observations for the next step.
    auto collect_obs = builder.addToGraph<ParallelForNode<Engine,
        collectObservationsSystem,
            Position,
            Rotation,
            GrabState,
            SelfObservation
        >>({post_reset_broadphase});

    // The lidar system
#ifdef MADRONA_GPU_MODE
    // Note the use of CustomParallelForNode to create a taskgraph node
    // that launches a warp of threads (32) for each invocation (1).
    // The 32, 1 parameters could be changed to 32, 32 to create a system
    // that cooperatively processes 32 entities within a warp.
    auto lidar = builder.addToGraph<CustomParallelForNode<Engine,
        lidarSystem, 32, 1,
#else
    auto lidar = builder.addToGraph<ParallelForNode<Engine,
        lidarSystem,
#endif
            Entity,
            Lidar
        >>({post_reset_broadphase});

    if (cfg.renderBridge) {
        RenderingSystem::setupTasks(builder, {reset_sys});
    }

#ifdef MADRONA_GPU_MODE
    // Sort entities, this could be conditional on reset like the second
    // BVH build above.
    auto sort_agents = queueSortByWorld<Agent>(
        builder, {lidar, collect_obs});
    auto sort_phys_objects = queueSortByWorld<PhysicsEntity>(
        builder, {sort_agents});
    (void)sort_phys_objects;
#else
    (void)lidar;
    (void)collect_obs;
#endif
}

Sim::Sim(Engine &ctx,
         const Config &cfg,
         const WorldInit &)
    : WorldBase(ctx)
{
    phys::PhysicsSystem::init(ctx, cfg.rigidBodyObjMgr,
        consts::deltaT, consts::numPhysicsSubsteps, -9.8f * math::up,
        consts::maxTotalEntities);

    initRandKey = cfg.initRandKey;
    autoReset = cfg.autoReset;

    enableRender = cfg.renderBridge != nullptr;

    if (enableRender) {
        RenderingSystem::init(ctx, cfg.renderBridge);
    }

    curWorldEpisode = 0;

    // Creates agents, walls, etc.
    createPersistentEntities(ctx);

    // Generate initial world state
    initWorld(ctx);
}

// This declaration is needed for the GPU backend in order to generate the
// CUDA kernel for world initialization, which needs to be specialized to the
// application's world data type (Sim) and config and initialization types.
// On the CPU it is a no-op.
MADRONA_BUILD_MWGPU_ENTRY(Engine, Sim, Sim::Config, Sim::WorldInit);

}
