#include "Profile.h"
#include "ProfileModel.h"
#include <AK/QuickSort.h>
#include <LibCore/CFile.h>
#include <stdio.h>

Profile::Profile(const JsonArray& json)
    : m_json(json)
{
    m_first_timestamp = m_json.at(0).as_object().get("timestamp").to_number<u64>();
    m_last_timestamp = m_json.at(m_json.size() - 1).as_object().get("timestamp").to_number<u64>();

    m_model = ProfileModel::create(*this);
    rebuild_tree();

    m_sample_data.ensure_capacity(m_json.size());
    m_json.for_each([&](const JsonValue& sample) {
        u64 timestamp = sample.as_object().get("timestamp").to_number<u64>() - m_first_timestamp;
        bool in_kernel = sample.as_object().get("frames").as_array().at(1).as_object().get("address").to_number<u32>() < (8 * MB);
        m_sample_data.append({ timestamp, in_kernel });
    });
}

Profile::~Profile()
{
}

GModel& Profile::model()
{
    return *m_model;
}

void Profile::rebuild_tree()
{
    NonnullRefPtrVector<ProfileNode> roots;

    auto find_or_create_root = [&roots](const String& symbol, u32 address, u32 offset, u64 timestamp) -> ProfileNode& {
        for (int i = 0; i < roots.size(); ++i) {
            auto& root = roots[i];
            if (root.symbol() == symbol) {
                return root;
            }
        }
        auto new_root = ProfileNode::create(symbol, address, offset, timestamp);
        roots.append(new_root);
        return new_root;
    };

    m_json.for_each([&](const JsonValue& sample) {
        if (has_timestamp_filter_range()) {
            auto timestamp = sample.as_object().get("timestamp").to_number<u64>();
            if (timestamp < m_timestamp_filter_range_start || timestamp > m_timestamp_filter_range_end)
                return;
        }

        auto frames_value = sample.as_object().get("frames");
        auto& frames = frames_value.as_array();
        ProfileNode* node = nullptr;
        for (int i = frames.size() - 1; i >= 0; --i) {
            auto& frame = frames.at(i);

            auto symbol = frame.as_object().get("symbol").as_string_or({});
            auto address = frame.as_object().get("address").as_u32();
            auto offset = frame.as_object().get("offset").as_u32();
            auto timestamp = frame.as_object().get("timestamp").to_number<u64>();

            if (symbol.is_empty())
                break;

            if (!node)
                node = &find_or_create_root(symbol, address, offset, timestamp);
            else
                node = &node->find_or_create_child(symbol, address, offset, timestamp);

            node->increment_sample_count();
        }
    });

    for (auto& root : roots) {
        root.sort_children();
    }

    m_roots = move(roots);
    m_model->update();
}

OwnPtr<Profile> Profile::load_from_file(const StringView& path)
{
    auto file = CFile::construct(path);
    if (!file->open(CIODevice::ReadOnly)) {
        fprintf(stderr, "Unable to open %s, error: %s\n", String(path).characters(), file->error_string());
        return nullptr;
    }

    auto json = JsonValue::from_string(file->read_all());
    if (!json.is_array()) {
        fprintf(stderr, "Invalid format (not a JSON array)\n");
        return nullptr;
    }

    auto& samples = json.as_array();
    if (samples.is_empty())
        return nullptr;

    return NonnullOwnPtr<Profile>(NonnullOwnPtr<Profile>::Adopt, *new Profile(move(samples)));
}

void ProfileNode::sort_children()
{
    quick_sort(m_children.begin(), m_children.end(), [](auto& a, auto& b) {
        return a->sample_count() >= b->sample_count();
    });

    for (auto& child : m_children)
        child->sort_children();
}

void Profile::set_timestamp_filter_range(u64 start, u64 end)
{
    if (m_has_timestamp_filter_range && m_timestamp_filter_range_start == start && m_timestamp_filter_range_end == end)
        return;
    m_has_timestamp_filter_range = true;

    m_timestamp_filter_range_start = min(start, end);
    m_timestamp_filter_range_end = max(start, end);

    rebuild_tree();
}

void Profile::clear_timestamp_filter_range()
{
    if (!m_has_timestamp_filter_range)
        return;
    m_has_timestamp_filter_range = false;
    rebuild_tree();
}
