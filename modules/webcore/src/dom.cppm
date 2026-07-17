// mbun.webcore.dom — compact DOM tree seam.
//
// Bun references: runtime/webcore and runtime/webview bindings.  The source
// checkout is not present in this worktree; this interface deliberately keeps
// the host/JSC binding behind WebCoreBackend instead of inventing a JS ABI.
export module mbun.webcore.dom;

import std;

export namespace mbun::webcore::dom {

using NodeId = std::uint32_t;
inline constexpr NodeId INVALID_NODE = 0;

enum class NodeType : std::uint8_t { document, element, text };

struct Attribute {
    std::string name;
    std::string value;
};

struct Node {
    NodeId id { INVALID_NODE };
    NodeType type { NodeType::document };
    std::string name;
    std::string value;
    NodeId parent { INVALID_NODE };
    std::vector<NodeId> children;
    std::vector<Attribute> attributes;
};

class Document {
private:
    std::vector<Node> nodes_;

    Node* find_(NodeId id) {
        if (id == INVALID_NODE || id > nodes_.size()) return nullptr;
        return &nodes_[id - 1];
    }

public:
    Document() { nodes_.push_back(Node { 1, NodeType::document, "#document" }); }

    NodeId root() const { return 1; }

    NodeId create_element(std::string_view name) {
        NodeId id { static_cast<NodeId>(nodes_.size() + 1) };
        nodes_.push_back(Node { id, NodeType::element, std::string(name) });
        return id;
    }

    NodeId create_text(std::string_view value) {
        NodeId id { static_cast<NodeId>(nodes_.size() + 1) };
        nodes_.push_back(Node { id, NodeType::text, "#text", std::string(value) });
        return id;
    }

    std::expected<void, std::string> append_child(NodeId parent, NodeId child) {
        auto* p = find_(parent);
        auto* c = find_(child);
        if (p == nullptr || c == nullptr) return std::unexpected("invalid node id");
        if (child == root() || parent == child) return std::unexpected("invalid tree relation");
        if (c->parent != INVALID_NODE) return std::unexpected("child already attached");
        for (NodeId ancestor { parent }; ancestor != INVALID_NODE;) {
            if (ancestor == child) return std::unexpected("cycle in tree");
            const auto* a = find_(ancestor);
            ancestor = a == nullptr ? INVALID_NODE : a->parent;
        }
        c->parent = parent;
        p->children.push_back(child);
        return {};
    }

    std::expected<void, std::string> set_attribute(NodeId id, std::string_view name,
                                                    std::string_view value) {
        auto* node = find_(id);
        if (node == nullptr || node->type != NodeType::element || name.empty())
            return std::unexpected("attributes require a named element");
        for (auto& attr : node->attributes) {
            if (attr.name == name) {
                attr.value = value;
                return {};
            }
        }
        node->attributes.push_back(Attribute { std::string(name), std::string(value) });
        return {};
    }

    const Node* get(NodeId id) const {
        if (id == INVALID_NODE || id > nodes_.size()) return nullptr;
        return &nodes_[id - 1];
    }
};

} // namespace mbun::webcore::dom
