#pragma once
// Doxygen module definitions — no runtime content.
// Add @addtogroup <name> / @{ / @} around a header's content to populate a group,
// or @ingroup <name> on individual classes/functions.

/** @defgroup core Core
 *  @brief SoA storage substrate: FieldProxy, component schema, temporal ring buffers,
 *         archetype chunks, logic-thread coordination, engine bootstrap.
 */

/** @defgroup gameplay Gameplay Framework
 *  @brief OOP layer over the ECS substrate: Construct<T>, ConstructView<TEntity>,
 *         Owned<T>, tick auto-registration, ConstructBatch dispatch.
 */

/** @defgroup flow Game Flow
 *  @brief Session lifecycle: FlowManager, GameState, GameMode, Soul,
 *         travel primitives, WithSpawnManagement / WithLobby / WithTeamAssignment mixins.
 */

/** @defgroup networking Networking
 *  @brief Authority/Owner net handlers, ServerClientChannel, entity replication,
 *         rollback netcode, four-phase networked despawn protocol.
 */

/** @defgroup rendering Rendering
 *  @brief Vulkan context, three-pass GPU compute pipeline (predicate → prefix_sum → scatter),
 *         dirty-bit-driven upload, InstanceBuffer double-buffering.
 */

/** @defgroup physics Physics
 *  @brief Jolt Physics integration: body management, step/pull loop, awake-only pull,
 *         JoltCharacter wrapper for CharacterVirtual controllers.
 */

/** @defgroup audio Audio
 *  @brief SDL3 voice pool, handle-based playback, event registry, priority voice stealing,
 *         fade/stop API. Anti-Event-compatible for rollback presentation reconciliation.
 */

/** @defgroup camera Camera
 *  @brief Per-Soul CameraLayer stack, slot resolution, orientation dispatch,
 *         CameraStateMix / CameraBlendMix / CameraOrientationMix CRTP mixins.
 */

/** @defgroup math Math & Determinism
 *  @brief Fixed32 fixed-point scalar (0.1 mm precision, bit-identical across platforms),
 *         SimFloat alias, FixedTrig, VecMath utilities.
 */

/** @defgroup containers Containers
 *  @brief Lock-free ring buffers (MPMC, MPSC, SPMC), FlatMap, FixedBitset —
 *         all optimised for the 512 Hz fixed-update hot path.
 */

/** @defgroup editor Editor
 *  @brief EditorContext, 8 editor panels, PIE (local + networked), ImGuizmo gizmo,
 *         50-command undo/redo stack, AssetDatabase (.tnxid sidecars, .tnxdb), GPU picking.
 */
