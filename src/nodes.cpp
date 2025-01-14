#include <algorithm>
#include <cmath>
#include <functional>
#include <optional>
#include <unordered_set>

#include <glm/gtc/matrix_transform.hpp>

#include "kaacore/engine.h"
#include "kaacore/exceptions.h"
#include "kaacore/geometry.h"
#include "kaacore/log.h"
#include "kaacore/nodes.h"
#include "kaacore/scenes.h"
#include "kaacore/shapes.h"
#include "kaacore/views.h"

namespace kaacore {

const int16_t default_root_z_index = 0;
const ViewIndexSet default_root_views =
    std::unordered_set<int16_t>{views_default_z_index};

Node::Node(NodeType type) : _type(type)
{
    if (type == NodeType::space) {
        new (&this->space) SpaceNode();
    } else if (type == NodeType::body) {
        new (&this->body) BodyNode();
    } else if (type == NodeType::hitbox) {
        new (&this->hitbox) HitboxNode();
        this->_color = {1., 0., 0., 0.};
    } else if (type == NodeType::text) {
        new (&this->text) TextNode();
        this->_origin_alignment = Alignment::center;
    }
}

Node::~Node()
{
    KAACORE_LOG_DEBUG("Destroying node: {}", fmt::ptr(this));
    if (this->_parent != nullptr) {
        auto pos_in_parent = std::find(
            this->_parent->_children.begin(), this->_parent->_children.end(),
            this);
        if (pos_in_parent != this->_parent->_children.end()) {
            this->_parent->_children.erase(pos_in_parent);
        }
    }

    while (not this->_children.empty()) {
        delete this->_children[0];
    }

    if (this->_type == NodeType::space) {
        if (this->_scene) {
            this->_scene->unregister_simulation(this);
        }
        this->space.~SpaceNode();
    } else if (this->_type == NodeType::body) {
        this->body.~BodyNode();
    } else if (this->_type == NodeType::hitbox) {
        this->hitbox.~HitboxNode();
    } else if (this->_type == NodeType::text) {
        this->text.~TextNode();
    }
}

void
Node::_mark_to_delete()
{
    if (this->_marked_to_delete) {
        return;
    }
    KAACORE_LOG_DEBUG("Marking node to delete: {}", fmt::ptr(this));
    KAACORE_ASSERT(this->_scene != nullptr, "Node not attached to the tree.");
    this->_marked_to_delete = true;
    if (this->_node_wrapper) {
        this->_node_wrapper->on_detach();
    }
    this->_scene->handle_remove_node_from_tree(this);
    // TODO ensure that parent can't access the removed node
    for (auto child : this->_children) {
        child->_mark_to_delete();
    }

    // Physics have side effect if we don't perform
    // deletion immediately, so we give it special
    // treatment here by detaching them from simulation.
    if (this->_type == NodeType::body) {
        this->body.detach_from_simulation();
    } else if (this->_type == NodeType::hitbox) {
        this->hitbox.detach_from_simulation();
    }
}

glm::fmat4
Node::_compute_model_matrix(const glm::fmat4& parent_matrix) const
{
    return glm::scale(
        glm::rotate(
            glm::translate(
                parent_matrix,
                glm::fvec3(this->_position.x, this->_position.y, 0.)),
            static_cast<float>(this->_rotation), glm::fvec3(0., 0., 1.)),
        glm::fvec3(this->_scale.x, this->_scale.y, 1.));
}

glm::fmat4
Node::_compute_model_matrix_cumulative(const Node* const ancestor) const
{
    const Node* pointer = this;
    std::vector<const Node*> inheritance_chain{pointer};
    while ((pointer = pointer->_parent) != ancestor) {
        if (pointer == nullptr) {
            throw kaacore::exception("Can't compute position relative to node "
                                     "that isn't its parent.");
        }
        inheritance_chain.push_back(pointer);
    }

    glm::fmat4 matrix(1.0);
    for (auto it = inheritance_chain.rbegin(); it != inheritance_chain.rend();
         it++) {
        matrix = (*it)->_compute_model_matrix(matrix);
    }
    return matrix;
}

void
Node::_recalculate_model_matrix()
{
    const static glm::fmat4 identity(1.0);
    this->_model_matrix.value = this->_compute_model_matrix(
        this->_parent ? this->_parent->_model_matrix.value : identity);
    this->clear_dirty_flags(DIRTY_MODEL_MATRIX_RECURSIVE);
}

void
Node::_recalculate_model_matrix_cumulative()
{
    Node* pointer = this;
    std::vector<Node*> inheritance_chain{pointer};
    while ((pointer = pointer->_parent) != nullptr and
           pointer->query_dirty_flags(DIRTY_MODEL_MATRIX)) {
        inheritance_chain.push_back(pointer);
    }

    for (auto it = inheritance_chain.rbegin(); it != inheritance_chain.rend();
         it++) {
        (*it)->_recalculate_model_matrix();
    }
}

void
Node::_set_position(const glm::dvec2& position)
{
    if (this->_position == position) {
        return;
    }
    this->set_dirty_flags(
        DIRTY_DRAW_VERTICES_RECURSIVE | DIRTY_SPATIAL_INDEX_RECURSIVE |
        DIRTY_MODEL_MATRIX_RECURSIVE);
    this->_position = position;
}

void
Node::_set_rotation(const double rotation)
{
    if (rotation == this->_rotation) {
        return;
    }
    this->set_dirty_flags(
        DIRTY_DRAW_VERTICES_RECURSIVE | DIRTY_SPATIAL_INDEX_RECURSIVE |
        DIRTY_MODEL_MATRIX_RECURSIVE);
    this->_rotation = rotation;
}

DrawBucketKey
Node::_make_draw_bucket_key() const
{
    DrawBucketKey key;
    key.views = this->_ordering_data.calculated_views;
    key.z_index = this->_ordering_data.calculated_z_index;
    key.root_distance = this->_root_distance;
    key.texture_raw_ptr = this->_sprite.texture.get();
    if (not this->_material and this->_type == NodeType::text) {
        key.material_raw_ptr = get_engine()->renderer->sdf_font_material.get();
    } else {
        key.material_raw_ptr = this->_material.get();
    }
    key.state_flags = 0u;
    key.stencil_flags = 0u;

    return key;
}

NodePtr
Node::add_child(NodeOwnerPtr& owned_ptr)
{
    KAACORE_CHECK(owned_ptr, "Cannot attach uninitialized/released node.");
    KAACORE_CHECK(owned_ptr->_parent == nullptr, "Node has a parent already.");

    auto child_node = owned_ptr.release();
    child_node->_parent = this;
    this->_children.push_back(child_node.get());

    if (child_node->_node_wrapper) {
        child_node->_node_wrapper->on_add_to_parent();
    }

    // TODO set root
    // TODO optimize (replace with iterator?)
    std::function<void(Node*)> initialize_node;
    initialize_node = [&initialize_node, this](Node* n) {
        n->_root_distance = n->_parent->_root_distance + 1;
        bool added_to_scene =
            (n->_scene == nullptr and this->_scene != nullptr);
        n->_scene = this->_scene;
        if (added_to_scene) {
            this->_scene->handle_add_node_to_tree(n);

            if (n->_node_wrapper) {
                n->_node_wrapper->on_attach();
            }

            if (n->_type == NodeType::space) {
                n->_scene->register_simulation(n);
            } else if (n->_type == NodeType::body) {
                n->body.attach_to_simulation();
            } else if (n->_type == NodeType::hitbox) {
                n->hitbox.attach_to_simulation();
            }
        }

        std::for_each(
            n->_children.begin(), n->_children.end(), initialize_node);
    };

    initialize_node(child_node.get());
    return child_node;
}

void
Node::recalculate_model_matrix()
{
    if (not this->query_dirty_flags(DIRTY_MODEL_MATRIX)) {
        return;
    }
    this->_recalculate_model_matrix();
}

VerticesIndicesVectorPair
Node::recalculate_vertices_indices_data()
{
    KAACORE_ASSERT(
        this->_shape,
        "Node has no shape set to calcualte vertices and indices data");

    std::vector<StandardVertexData> computed_vertices;
    computed_vertices.resize(this->_shape.vertices.size());

    glm::dvec2 pos_realignment = calculate_realignment_vector(
        this->_origin_alignment, this->_shape.vertices_bbox);

    std::optional<std::pair<glm::dvec2, glm::dvec2>> uv_rect;
    if (this->_sprite.has_texture()) {
        uv_rect = this->_sprite.get_display_rect();
    }

    std::transform(
        this->_shape.vertices.cbegin(), this->_shape.vertices.cend(),
        computed_vertices.begin(),
        [this, &uv_rect, pos_realignment](
            const StandardVertexData& orig_vt) -> StandardVertexData {
            StandardVertexData vt;
            vt.xyz =
                this->_model_matrix.value *
                (glm::fvec4{orig_vt.xyz.x, orig_vt.xyz.y, orig_vt.xyz.z, 1.} +
                 glm::fvec4{pos_realignment.x, pos_realignment.y, 0., 0.});

            if (uv_rect) {
                vt.uv = glm::mix(uv_rect->first, uv_rect->second, orig_vt.uv);
            }
            vt.mn = orig_vt.mn;
            vt.rgba *= this->_color;
            return vt;
        });

    return {computed_vertices, this->_shape.indices};
}

void
Node::recalculate_ordering_data()
{
    if (not this->query_dirty_flags(DIRTY_ORDERING)) {
        return;
    }

    if (this->_views.has_value()) {
        this->_ordering_data.calculated_views = *this->_views;
    } else if (this->is_root()) {
        this->_ordering_data.calculated_views = default_root_views;
    } else {
        KAACORE_ASSERT(
            this->_parent != nullptr,
            "Can't inherit view data if node has no parent");
        this->_parent->recalculate_ordering_data();
        this->_ordering_data.calculated_views =
            this->_parent->_ordering_data.calculated_views;
    }

    if (this->_z_index.has_value()) {
        this->_ordering_data.calculated_z_index = *this->_z_index;
    } else if (this->is_root()) {
        this->_ordering_data.calculated_z_index = default_root_z_index;
    } else {
        KAACORE_ASSERT(
            this->_parent != nullptr,
            "Can't inherit z_index data if node has no parent");
        this->_ordering_data.calculated_z_index =
            this->_parent->_ordering_data.calculated_z_index;
    }
    this->clear_dirty_flags(DIRTY_ORDERING_RECURSIVE);
}

void
Node::recalculate_visibility_data()
{
    if (not this->query_dirty_flags(DIRTY_VISIBILITY)) {
        return;
    }

    const auto& inheritance_chain = this->build_inheritance_chain(
        [](Node* n) { return n->query_dirty_flags(DIRTY_VISIBILITY); });

    for (auto it = inheritance_chain.rbegin(); it != inheritance_chain.rend();
         it++) {
        Node* node = *it;
        if (not node->_visible) {
            node->_visibility_data.calculated_visible = false;
        } else if (node->is_root()) {
            node->_visibility_data.calculated_visible = true;
        } else {
            KAACORE_ASSERT(
                node->_parent != nullptr,
                "Nodes ({}) parent is not set, cannot determine "
                "inheritance-based value",
                fmt::ptr(node));
            node->_visibility_data.calculated_visible =
                node->_parent->_visibility_data.calculated_visible;
        }

        node->clear_dirty_flags(DIRTY_VISIBILITY_RECURSIVE);
    }
}

std::optional<DrawUnitModification>
Node::calculate_draw_unit_removal() const
{
    KAACORE_ASSERT(
        this->_scene_tree_id != 0u, "Node ({}) has no scene tree id",
        fmt::ptr(this));
    if (not this->_draw_unit_data.current_key) {
        return std::nullopt;
    }

    return DrawUnitModification{DrawUnitModification::Type::remove,
                                *this->_draw_unit_data.current_key,
                                this->_scene_tree_id};
}

DrawUnitModificationPack
Node::calculate_draw_unit_updates()
{
    KAACORE_ASSERT(
        this->_scene_tree_id != 0u, "Node ({}) has no scene tree id",
        fmt::ptr(this));

    if (not this->query_dirty_flags(DIRTY_DRAW_KEYS | DIRTY_DRAW_VERTICES)) {
        return {std::nullopt, std::nullopt};
    }

    this->recalculate_model_matrix();
    this->recalculate_ordering_data();
    this->recalculate_visibility_data();

    const bool is_visible =
        this->_shape and this->_visibility_data.calculated_visible;
    const std::optional<DrawBucketKey> calculated_draw_bucket_key =
        is_visible ? std::optional<DrawBucketKey>{this->_make_draw_bucket_key()}
                   : std::nullopt;
    const bool changed_draw_bucket_key =
        this->query_dirty_flags(DIRTY_DRAW_KEYS) and
        this->_draw_unit_data.current_key != calculated_draw_bucket_key;
    const bool changed_vertices_indices =
        this->query_dirty_flags(DIRTY_DRAW_VERTICES);

    std::optional<DrawUnitModification> remove_mod{std::nullopt};
    std::optional<DrawUnitModification> upsert_mod{std::nullopt};

    if (changed_draw_bucket_key and this->_draw_unit_data.current_key) {
        // draw unit needs to be removed from previous bucket
        remove_mod = this->calculate_draw_unit_removal().value();
    }

    // skip inserting/updating if node will not be rendered
    if (is_visible and (changed_draw_bucket_key or changed_vertices_indices)) {
        upsert_mod = DrawUnitModification{
            changed_draw_bucket_key ? DrawUnitModification::Type::insert
                                    : DrawUnitModification::Type::update,
            *calculated_draw_bucket_key, this->_scene_tree_id};

        upsert_mod->updated_vertices_indices = true;
        auto vertices_indices_pair = this->recalculate_vertices_indices_data();
        upsert_mod->state_update.vertices =
            std::move(vertices_indices_pair.first);
        upsert_mod->state_update.indices =
            std::move(vertices_indices_pair.second);
    }

    return {upsert_mod, remove_mod};
}

void
Node::clear_draw_unit_updates(const std::optional<const DrawBucketKey> key)
{
    this->_draw_unit_data.current_key = key;
}

void
Node::set_dirty_flags(const Node::DirtyFlagsType flags)
{
    // Check which recursive flags are not currently set on targetted node.
    // If those flags are set already it is guaranteed that all of
    // the children will have those flags set as well.
    auto unapplied_recursive_flags =
        flags & ~this->_dirty_flags & DIRTY_ANY_RECURSIVE;
    this->_dirty_flags |= flags;

    if (unapplied_recursive_flags.any()) {
        // promote `recursive` flags to `non-recursive` variant
        auto children_flags =
            unapplied_recursive_flags |
            unapplied_recursive_flags >> DIRTY_FLAGS_SHIFT_RECURSIVE;

        this->recursive_call_downstream_children([children_flags](Node* node) {
            // repeated check if any recursive flags are not applied,
            // this time on child level
            bool descend =
                (children_flags & ~node->_dirty_flags & DIRTY_ANY_RECURSIVE)
                    .any();
            node->_dirty_flags |= children_flags;
            return descend;
        });
    }
}

void
Node::clear_dirty_flags(const Node::DirtyFlagsType flags)
{
    this->_dirty_flags &= ~flags;
}

bool
Node::query_dirty_flags(const Node::DirtyFlagsType flags)
{
    return (this->_dirty_flags & flags).any();
}

const NodeType
Node::type() const
{
    return this->_type;
}

std::vector<Node*>
Node::children()
{
    std::vector<Node*> result;
    for (auto node : this->_children) {
        if (not node->_marked_to_delete) {
            result.push_back(node);
        }
    }
    return result;
}

bool
Node::is_root() const
{
    return (this->_scene != nullptr and &this->_scene->root_node == this);
}

glm::dvec2
Node::position()
{
    return this->_position;
}

void
Node::position(const glm::dvec2& position)
{
    this->_set_position(position);
    if (this->_type == NodeType::body) {
        this->body.override_simulation_position();
    } else if (this->_in_hitbox_chain) {
        this->_update_hitboxes();
    }
}

glm::dvec2
Node::absolute_position()
{
    if (this->query_dirty_flags(DIRTY_MODEL_MATRIX)) {
        this->_recalculate_model_matrix_cumulative();
    }

    glm::fvec4 pos = {0., 0., 0., 1.};
    pos = this->_model_matrix.value * pos;
    return {pos.x, pos.y};
}

glm::dvec2
Node::get_relative_position(const Node* const ancestor)
{
    if (ancestor == this->_parent) {
        return this->position();
    } else if (ancestor == nullptr) {
        return this->absolute_position();
    } else if (ancestor == this) {
        return {0., 0.};
    }

    glm::fvec4 pos = {0., 0., 0., 1.};
    pos = this->_compute_model_matrix_cumulative(ancestor) * pos;
    return {pos.x, pos.y};
}

double
Node::rotation()
{
    return this->_rotation;
}

double
Node::absolute_rotation()
{
    if (this->query_dirty_flags(DIRTY_MODEL_MATRIX)) {
        this->_recalculate_model_matrix_cumulative();
    }

    return DecomposedTransformation<float>(this->_model_matrix.value).rotation;
}

void
Node::rotation(const double& rotation)
{
    this->_set_rotation(rotation);
    if (this->_type == NodeType::body) {
        this->body.override_simulation_rotation();
    } else if (this->_in_hitbox_chain) {
        this->_update_hitboxes();
    }
}

glm::dvec2
Node::scale()
{
    return this->_scale;
}

glm::dvec2
Node::absolute_scale()
{
    if (this->query_dirty_flags(DIRTY_MODEL_MATRIX)) {
        this->_recalculate_model_matrix_cumulative();
    }

    return DecomposedTransformation<float>(this->_model_matrix.value).scale;
}

void
Node::scale(const glm::dvec2& scale)
{
    if (scale == this->_scale) {
        return;
    }
    this->set_dirty_flags(
        DIRTY_DRAW_VERTICES_RECURSIVE | DIRTY_SPATIAL_INDEX_RECURSIVE |
        DIRTY_MODEL_MATRIX_RECURSIVE);
    this->_scale = scale;

    auto body_in_tree = this->_type == NodeType::body and this->_scene;
    if (body_in_tree or this->_in_hitbox_chain) {
        this->_update_hitboxes();
    }
}

Transformation
Node::absolute_transformation()
{
    if (this->query_dirty_flags(DIRTY_MODEL_MATRIX)) {
        this->_recalculate_model_matrix_cumulative();
    }
    return Transformation{this->_model_matrix.value};
}

Transformation
Node::get_relative_transformation(const Node* const ancestor)
{
    if (ancestor == nullptr) {
        return this->absolute_transformation();
    } else if (ancestor == this) {
        return Transformation{glm::dmat4(1.)};
    }

    return Transformation{this->_compute_model_matrix_cumulative(ancestor)};
}

Transformation
Node::transformation()
{
    return this->get_relative_transformation(this->_parent);
}

void
Node::transformation(const Transformation& transformation)
{
    auto decomposed = transformation.decompose();
    this->position(decomposed.translation);
    this->rotation(decomposed.rotation);
    this->scale(decomposed.scale);
}

std::optional<int16_t>
Node::z_index()
{
    return this->_z_index;
}

void
Node::z_index(const std::optional<int16_t>& z_index)
{
    if (this->_z_index == z_index) {
        return;
    }
    this->_z_index = z_index;
    this->set_dirty_flags(DIRTY_DRAW_KEYS_RECURSIVE | DIRTY_ORDERING_RECURSIVE);
}

int16_t
Node::effective_z_index()
{
    this->recalculate_ordering_data();
    return this->_ordering_data.calculated_z_index;
}

Shape
Node::shape()
{
    return this->_shape;
}

void
Node::shape(const Shape& shape)
{
    this->shape(shape, false);
}

void
Node::shape(const Shape& shape, bool is_auto_shape)
{
    if (this->_shape == shape) {
        return;
    }
    this->_shape = shape;
    if (not shape) {
        this->_auto_shape = true;
    } else {
        this->_auto_shape = is_auto_shape;
    }

    if (this->_type == NodeType::hitbox) {
        this->hitbox.update_physics_shape();
    }
    this->set_dirty_flags(DIRTY_DRAW_VERTICES | DIRTY_SPATIAL_INDEX);
}

Sprite
Node::sprite()
{
    return this->_sprite;
}

void
Node::sprite(const Sprite& sprite)
{
    if (this->_sprite == sprite) {
        return;
    }
    if (this->_sprite.texture != sprite.texture) {
        this->set_dirty_flags(DIRTY_DRAW_KEYS);
    }
    this->set_dirty_flags(DIRTY_DRAW_VERTICES);

    this->_sprite = sprite;
    if (this->_auto_shape) {
        if (sprite) {
            this->shape(Shape::Box(sprite.get_size()), true);
        } else {
            this->shape(Shape{});
        }
    }
}

ResourceReference<Material>&
Node::material()
{
    return this->_material;
}

void
Node::material(const ResourceReference<Material>& material)
{
    this->_material = material;
}

glm::dvec4
Node::color()
{
    return this->_color;
}

void
Node::color(const glm::dvec4& color)
{
    if (color == this->_color) {
        return;
    }
    this->set_dirty_flags(DIRTY_DRAW_VERTICES);
    this->_color = color;
}

bool
Node::visible()
{
    return this->_visible;
}

void
Node::visible(const bool& visible)
{
    if (visible == this->_visible) {
        return;
    }
    this->set_dirty_flags(
        DIRTY_DRAW_KEYS_RECURSIVE | DIRTY_VISIBILITY_RECURSIVE);
    this->_visible = visible;
}

Alignment
Node::origin_alignment()
{
    return this->_origin_alignment;
}

void
Node::origin_alignment(const Alignment& alignment)
{
    if (alignment == this->_origin_alignment) {
        return;
    }
    this->set_dirty_flags(DIRTY_DRAW_VERTICES);
    this->_origin_alignment = alignment;
}

NodeTransitionHandle
Node::transition()
{
    return this->_transitions_manager.get(default_transition_name);
}

void
Node::transition(const NodeTransitionHandle& transition)
{
    this->_transitions_manager.set(default_transition_name, transition);
}

Duration
Node::lifetime()
{
    return this->_lifetime;
}

void
Node::lifetime(const Duration& lifetime)
{
    this->_lifetime =
        std::chrono::duration_cast<HighPrecisionDuration>(lifetime);
}

NodeTransitionsManager&
Node::transitions_manager()
{
    return this->_transitions_manager;
}

Scene* const
Node::scene() const
{
    return this->_scene;
}

NodePtr
Node::parent() const
{
    return this->_parent;
}

void
Node::views(const std::optional<std::unordered_set<int16_t>>& z_indices)
{
    if (z_indices.has_value()) {
        KAACORE_CHECK(
            z_indices->size() <= KAACORE_MAX_VIEWS, "Invalid indices size.");
    }

    this->set_dirty_flags(DIRTY_DRAW_KEYS_RECURSIVE | DIRTY_ORDERING_RECURSIVE);
    this->_views = z_indices;
}

const std::optional<std::vector<int16_t>>
Node::views() const
{
    return this->_views;
}

const std::vector<int16_t>
Node::effective_views()
{
    this->recalculate_ordering_data();
    return this->_ordering_data.calculated_views;
}

void
Node::setup_wrapper(std::unique_ptr<ForeignNodeWrapper>&& wrapper)
{
    KAACORE_ASSERT(!this->_node_wrapper, "Node wrapper already initialized.");
    this->_node_wrapper = std::move(wrapper);
}

ForeignNodeWrapper*
Node::wrapper_ptr() const
{
    return this->_node_wrapper.get();
}

void
Node::indexable(const bool indexable_flag)
{
    if (this->_indexable == indexable_flag) {
        return;
    }
    this->_indexable = indexable_flag;
    this->set_dirty_flags(DIRTY_SPATIAL_INDEX);
}

bool
Node::indexable() const
{
    return this->_indexable;
}

uint16_t
Node::root_distance() const
{
    return this->_root_distance;
}

uint64_t
Node::scene_tree_id() const
{
    return this->_scene_tree_id;
}

BoundingBox<double>
Node::bounding_box()
{
    const auto transformation = this->absolute_transformation();

    if (this->_shape) {
        KAACORE_ASSERT(
            not this->_shape.bounding_points.empty(),
            "Shape must have bounding points");
        std::vector<glm::dvec2> bounding_points;
        bounding_points.resize(this->_shape.bounding_points.size());
        std::transform(
            this->_shape.bounding_points.begin(),
            this->_shape.bounding_points.end(), bounding_points.begin(),
            [&transformation](glm::dvec2 pt) -> glm::dvec2 {
                return pt | transformation;
            });
        return BoundingBox<double>::from_points(bounding_points);
    } else {
        return BoundingBox<double>::single_point(
            this->_position | transformation);
    }
}

void
Node::_update_hitboxes()
{
    this->recursive_call_downstream([](Node* n) {
        if (n->_type == NodeType::hitbox) {
            n->hitbox.update_physics_shape();
        }
        return n->_in_hitbox_chain;
    });
}

} // namespace kaacore
