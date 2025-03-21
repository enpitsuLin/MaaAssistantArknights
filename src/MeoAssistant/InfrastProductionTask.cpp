#include "InfrastProductionTask.h"

#include <algorithm>

#include <calculator/calculator.hpp>

#include "Controller.h"
#include "InfrastOperImageAnalyzer.h"
#include "HashImageAnalyzer.h"
#include "Logger.hpp"
#include "MatchImageAnalyzer.h"
#include "MultiMatchImageAnalyzer.h"
#include "OcrWithPreprocessImageAnalyzer.h"
#include "Resource.h"
#include "RuntimeStatus.h"
#include "ProcessTask.h"

asst::InfrastProductionTask& asst::InfrastProductionTask::set_uses_of_drone(std::string uses_of_drones) noexcept
{
    m_uses_of_drones = std::move(uses_of_drones);
    return *this;
}

std::string asst::InfrastProductionTask::get_uses_of_drone() const noexcept
{
    return m_uses_of_drones;
}

void asst::InfrastProductionTask::set_product(std::string product_name) noexcept
{
    m_product = std::move(product_name);

    json::value callback_info = basic_info_with_what("ProductOfFacility");
    callback_info["details"]["product"] = m_product;
    // 该回调注册了插件 DronesForShamareTaskPlugin
    callback(AsstMsg::SubTaskExtraInfo, callback_info);
}

bool asst::InfrastProductionTask::shift_facility_list()
{
    LogTraceFunction;
    if (!facility_list_detect() || need_exit()) {
        return false;
    }
    const auto tab_task_ptr = Task.get("InfrastFacilityListTab" + facility_name());
    MatchImageAnalyzer add_analyzer;

    const auto add_task_ptr = Task.get("InfrastAddOperator" + facility_name() + m_work_mode_name);
    add_analyzer.set_task_info(add_task_ptr);
    MultiMatchImageAnalyzer locked_analyzer;

    locked_analyzer.set_task_info("InfrastOperLocked" + facility_name());

    for (; m_cur_facility_index < m_facility_list_tabs.size(); ++m_cur_facility_index) {
        Rect tab = m_facility_list_tabs.at(m_cur_facility_index);
        if (need_exit()) {
            return false;
        }
        if (m_cur_facility_index != 0) {
            callback(AsstMsg::SubTaskExtraInfo, basic_info_with_what("EnterFacility"));
        }

        m_ctrler->click(tab);
        sleep(tab_task_ptr->rear_delay);

        /* 识别当前制造/贸易站有没有添加干员按钮，没有就不换班 */
        const auto image = m_ctrler->get_image();
        add_analyzer.set_image(image);
        if (!add_analyzer.analyze()) {
            Log.info("no add button, just continue");
            continue;
        }
        auto& rect = add_analyzer.get_result().rect;
        Rect add_button = rect;
        auto& rect_move = add_task_ptr->rect_move;
        if (!rect_move.empty()) {
            add_button.x += rect_move.x;
            add_button.y += rect_move.y;
            add_button.width = rect_move.width;
            add_button.height = rect_move.height;
        }

        /* 识别当前正在造什么 */
        MatchImageAnalyzer product_analyzer(image);

        auto& all_products = Resrc.infrast().get_facility_info(facility_name()).products;
        std::string cur_product = all_products.at(0);
        double max_score = 0;
        for (const std::string& product : all_products) {
            product_analyzer.set_task_info("InfrastFlag" + product);
            if (product_analyzer.analyze()) {
                double score = product_analyzer.get_result().score;
                if (score > max_score) {
                    max_score = score;
                    cur_product = product;
                }
            }
        }
        set_product(cur_product);

        locked_analyzer.set_image(image);
        if (locked_analyzer.analyze()) {
            m_cur_num_of_lokced_opers = static_cast<int>(locked_analyzer.get_result().size());
        }
        else {
            m_cur_num_of_lokced_opers = 0;
        }

        /* 进入干员选择页面 */
        m_ctrler->click(add_button);
        sleep(add_task_ptr->rear_delay);

        for (int i = 0; i <= OperSelectRetryTimes; ++i) {
            if (need_exit()) {
                return false;
            }
            click_clear_button();

            if (m_all_available_opers.empty()) {
                if (!opers_detect_with_swipe()) {
                    return false;
                }
                swipe_to_the_left_of_operlist(2);
            }
            else {
                opers_detect();
            }
            optimal_calc();
            if (!opers_choose()) {
                m_all_available_opers.clear();
                swipe_to_the_left_of_operlist(2);
                continue;
            }
            break;
        }
        click_confirm_button();

        // 使用无人机
        if (cur_product == m_uses_of_drones) {
            if (use_drone()) {
                m_uses_of_drones = "_Used";
            }
        }
    }
    return true;
}

bool asst::InfrastProductionTask::opers_detect_with_swipe()
{
    LogTraceFunction;
    m_all_available_opers.clear();

    while (true) {
        if (need_exit()) {
            return false;
        }
        size_t num = opers_detect();
        Log.trace("opers_detect return", num);

        if (num == 0) {
            break;
        }

        // 异步在最后会多滑动一下，耽误的时间还不如用同步
        swipe_of_operlist();
    }

    if (!m_all_available_opers.empty()) {
        return true;
    }
    return false;
}

size_t asst::InfrastProductionTask::opers_detect()
{
    LogTraceFunction;
    const auto image = m_ctrler->get_image();

    InfrastOperImageAnalyzer oper_analyzer(image);

    oper_analyzer.set_facility(facility_name());

    if (!oper_analyzer.analyze()) {
        return 0;
    }
    const auto& cur_all_opers = oper_analyzer.get_result();
    max_num_of_opers_per_page = (std::max)(max_num_of_opers_per_page, cur_all_opers.size());

    const int face_hash_thres = std::dynamic_pointer_cast<HashTaskInfo>(
        Task.get("InfrastOperFaceHash"))->dist_threshold;
    const size_t pre_size = m_all_available_opers.size();
    for (const auto& cur_oper : cur_all_opers) {
        if (cur_oper.skills.empty()) {
            continue;
        }
        {
            std::string skills_str = "[";
            for (const auto& skill : cur_oper.skills) {
                skills_str += skill.id + ", ";
            }
            skills_str += "]";
            Log.trace(skills_str, "mood", cur_oper.mood_ratio, "threshold", m_mood_threshold);
        }
        // 心情过低的干员则不可用
        if (cur_oper.mood_ratio < m_mood_threshold) {
            //--cur_available_num;
            continue;
        }
        auto find_iter = std::find_if(
            m_all_available_opers.cbegin(), m_all_available_opers.cend(),
            [&](const infrast::Oper& oper) -> bool {
                if (oper.skills != cur_oper.skills) {
                    return false;
                }
                // 有可能是同一个干员，比一下hash
                int dist = HashImageAnalyzer::hamming(cur_oper.face_hash, oper.face_hash);
                Log.debug("opers_detect hash dist |", dist);
                return dist < face_hash_thres;
            });
        // 如果两个的hash距离过小，则认为是同一个干员，不进行插入
        if (find_iter != m_all_available_opers.cend()) {
            continue;
        }
        m_all_available_opers.emplace_back(cur_oper);
    }
    return m_all_available_opers.size() - pre_size;
}

bool asst::InfrastProductionTask::optimal_calc()
{
    LogTraceFunction;
    auto& facility_info = Resrc.infrast().get_facility_info(facility_name());
    int cur_max_num_of_opers = facility_info.max_num_of_opers - m_cur_num_of_lokced_opers;

    std::vector<infrast::SkillsComb> all_avaliable_combs;
    all_avaliable_combs.reserve(m_all_available_opers.size());
    for (auto&& oper : m_all_available_opers) {
        auto comb = efficient_regex_calc(oper.skills);
        comb.name_img = oper.name_img;
        all_avaliable_combs.emplace_back(std::move(comb));
    }

    // 先把单个的技能按效率排个序，取效率最高的几个
    std::vector<infrast::SkillsComb> optimal_combs;
    optimal_combs.reserve(cur_max_num_of_opers);
    double max_efficient = 0;
    std::sort(all_avaliable_combs.begin(), all_avaliable_combs.end(),
        [&](const infrast::SkillsComb& lhs, const infrast::SkillsComb& rhs) -> bool {
            return lhs.efficient.at(m_product) > rhs.efficient.at(m_product);
        });

    for (const auto& comb : all_avaliable_combs) {
        std::string skill_str;
        for (const auto& skill : comb.skills) {
            skill_str += skill.id + " ";
        }
        Log.trace(skill_str, comb.efficient.at(m_product));
    }

    std::unordered_map<std::string, int> skills_num;
    for (int i = 0; i != m_all_available_opers.size(); ++i) {
        const auto& comb = all_avaliable_combs.at(i);

        bool out_of_num = false;
        for (auto&& skill : comb.skills) {
            if (skills_num[skill.id] >= skill.max_num) {
                out_of_num = true;
                break;
            }
        }
        if (out_of_num) {
            continue;
        }

        optimal_combs.emplace_back(comb);
        max_efficient += all_avaliable_combs.at(i).efficient.at(m_product);

        for (auto&& skill : comb.skills) {
            ++skills_num[skill.id];
        }

        if (optimal_combs.size() >= cur_max_num_of_opers) {
            break;
        }
    }

    {
        std::string log_str = "[ ";
        for (const auto& comb : optimal_combs) {
            log_str += comb.desc.empty() ? comb.skills.begin()->names.front() : comb.desc;
            log_str += "; ";
        }
        log_str += "]";
        Log.trace("Single comb efficient", max_efficient, " , skills:", log_str);
    }

    // 如果有被锁住的干员，说明当前基建没升满级，组合就不启用
    if (m_cur_num_of_lokced_opers != 0) {
        m_optimal_combs = std::move(optimal_combs);
        return true;
    }

    // 遍历所有组合，找到效率最高的
    auto& all_group = Resrc.infrast().get_skills_group(facility_name());
    for (const infrast::SkillsGroup& group : all_group) {
        Log.trace(group.desc);
        auto cur_available_opers = all_avaliable_combs;
        bool group_unavailable = false;
        std::vector<infrast::SkillsComb> cur_combs;
        cur_combs.reserve(cur_max_num_of_opers);
        double cur_efficient = 0;
        // 条件判断，不符合的直接过滤掉
        bool meet_condition = true;
        for (const auto& [cond, cond_value] : group.conditions) {
            auto cond_opt = m_status->get_number(cond);
            if (!cond_opt) {
                continue;
            }
            // TODO：这里做成除了不等于，还可计算大于、小于等不同条件的
            int cur_value = static_cast<int>(cond_opt.value());
            if (cur_value != cond_value) {
                meet_condition = false;
                break;
            }
        }
        if (!meet_condition) {
            continue;
        }
        // necessary里的技能，一个都不能少
        // TODO necessary暂时没做hash校验。因为没有需要比hash的necessary干员（
        for (const infrast::SkillsComb& nec_skills : group.necessary) {
            auto find_iter = std::find_if(
                cur_available_opers.cbegin(), cur_available_opers.cend(),
                [&](const infrast::SkillsComb& arg) -> bool {
                    return arg == nec_skills;
                });
            if (find_iter == cur_available_opers.cend()) {
                group_unavailable = true;
                break;
            }
            cur_combs.emplace_back(nec_skills);
            if (auto iter = nec_skills.efficient_regex.find(m_product);
                iter != nec_skills.efficient_regex.cend()) {
                cur_efficient += efficient_regex_calc(nec_skills.skills).efficient.at(m_product);
            }
            else {
                cur_efficient += nec_skills.efficient.at(m_product);
            }
            cur_available_opers.erase(find_iter);
        }
        if (group_unavailable) {
            continue;
        }
        // 排个序，因为产物不同，效率可能会发生变化，所以配置文件里默认的顺序不一定准确
        auto optional = group.optional;
        for (auto&& opt : optional) {
            if (auto iter = opt.efficient_regex.find(m_product);
                iter != opt.efficient_regex.cend()) {
                opt = efficient_regex_calc(opt.skills);
            }
        }

        std::sort(optional.begin(), optional.end(),
            [&](const infrast::SkillsComb& lhs,
                const infrast::SkillsComb& rhs) -> bool {
                    return lhs.efficient.at(m_product) > rhs.efficient.at(m_product);
            });

        // 可能有多个干员有同样的技能，所以这里需要循环找同一个技能，直到找不到为止
        for (const infrast::SkillsComb& opt : optional) {
            auto find_iter = cur_available_opers.cbegin();
            while (cur_combs.size() != cur_max_num_of_opers) {
                find_iter = std::find_if(
                    find_iter, cur_available_opers.cend(),
                    [&](const infrast::SkillsComb& arg) -> bool {
                        return arg == opt;
                    });
                if (find_iter != cur_available_opers.cend()) {
                    bool hash_matched = false;
                    if (opt.name_filter.empty()) {
                        hash_matched = true;
                    }
                    else {
                        OcrWithPreprocessImageAnalyzer name_analyzer(find_iter->name_img);
                        name_analyzer.set_replace(
                            Task.get<OcrTaskInfo>("CharsNameOcrReplace")->replace_map);
                        Log.trace("Analyze name filter");
                        if (!name_analyzer.analyze()) {
                            continue;
                        }
                        std::string name = name_analyzer.get_result().front().text;
                        hash_matched = std::find(
                            opt.name_filter.cbegin(), opt.name_filter.cend(), name)
                            != opt.name_filter.cend();
                    }
                    if (!hash_matched) {
                        ++find_iter;
                        continue;
                    }

                    cur_combs.emplace_back(opt);
                    cur_efficient += opt.efficient.at(m_product);
                    find_iter = cur_available_opers.erase(find_iter);
                }
                else {
                    break;
                }
            }
        }

        // 说明可选的没凑满人
        if (cur_combs.size() < cur_max_num_of_opers) {
            // 允许外部的话，就把单个干员凑进来
            if (group.allow_external) {
                for (size_t i = cur_combs.size(); i != cur_max_num_of_opers; ++i) {
                    size_t index = i - cur_combs.size();
                    cur_combs.emplace_back(cur_available_opers.at(index));
                    cur_efficient += cur_available_opers.at(index).efficient.at(m_product);
                }
            }
            else { // 否则这个组合人不够，就不可用了
                continue;
            }
        }
        {
            std::string log_str = "[ ";
            for (const auto& comb : cur_combs) {
                log_str += comb.desc.empty() ? comb.skills.begin()->names.front() : comb.desc;
                log_str += "; ";
            }
            log_str += "]";
            Log.trace(group.desc, "efficient", cur_efficient, " , skills:", log_str);
        }

        if (cur_efficient > max_efficient) {
            optimal_combs = std::move(cur_combs);
            max_efficient = cur_efficient;
        }
    }
    {
        std::string log_str = "[ ";
        for (const auto& comb : optimal_combs) {
            log_str += comb.desc.empty() ? comb.skills.begin()->names.front() : comb.desc;
            log_str += "; ";
        }
        log_str += "]";
        Log.trace("optimal efficient", max_efficient, " , skills:", log_str);
    }

    m_optimal_combs = std::move(optimal_combs);

    return true;
}

bool asst::InfrastProductionTask::opers_choose()
{
    LogTraceFunction;
    bool has_error = false;

    auto& facility_info = Resrc.infrast().get_facility_info(facility_name());
    int cur_max_num_of_opers = facility_info.max_num_of_opers - m_cur_num_of_lokced_opers;

    const int face_hash_thres = std::dynamic_pointer_cast<HashTaskInfo>(
        Task.get("InfrastOperFaceHash"))->dist_threshold;

    int count = 0;

    while (true) {
        if (need_exit()) {
            return false;
        }
        const auto image = m_ctrler->get_image();

        InfrastOperImageAnalyzer oper_analyzer(image);

        oper_analyzer.set_facility(facility_name());

        if (!oper_analyzer.analyze()) {
            return false;
        }
        oper_analyzer.sort_by_loc();

        // 这个情况一般是滑动/识别出错了，把所有的干员都滑过去了
        if (oper_analyzer.get_num_of_opers_with_skills() == 0) {
            if (!has_error) {
                has_error = true;
                // 倒回去再来一遍
                swipe_to_the_left_of_operlist();
                continue;
            }
            else {
                // 如果已经出过一次错了，那就可能不是opers_choose出错，而是之前的opers_detect出错了
                return false;
            }
        }
        auto cur_all_opers = oper_analyzer.get_result();
        Log.trace("before mood filter, opers size:", cur_all_opers.size());
        // 小于心情阈值的干员则不可用
        auto remove_iter = std::remove_if(cur_all_opers.begin(), cur_all_opers.end(),
            [&](const infrast::Oper& rhs) -> bool {
                return rhs.mood_ratio < m_mood_threshold;
            });
        cur_all_opers.erase(remove_iter, cur_all_opers.end());
        Log.trace("after mood filter, opers size:", cur_all_opers.size());
        for (auto opt_iter = m_optimal_combs.begin(); opt_iter != m_optimal_combs.end();) {
            Log.trace("to find", opt_iter->skills.begin()->names.front());
            auto find_iter = std::find_if(
                cur_all_opers.cbegin(), cur_all_opers.cend(),
                [&](const infrast::Oper& lhs) -> bool {
                    if (lhs.skills != opt_iter->skills) {
                        return false;
                    }
                    if (opt_iter->name_filter.empty()) {
                        return true;
                    }
                    else {
                        OcrWithPreprocessImageAnalyzer name_analyzer(lhs.name_img);
                        name_analyzer.set_replace(
                            Task.get<OcrTaskInfo>("CharsNameOcrReplace")->replace_map);
                        Log.trace("Analyze name filter");
                        if (!name_analyzer.analyze()) {
                            return false;
                        }
                        std::string name = name_analyzer.get_result().front().text;
                        return std::find(
                            opt_iter->name_filter.cbegin(), opt_iter->name_filter.cend(), name)
                            != opt_iter->name_filter.cend();
                    }
                });

            if (find_iter == cur_all_opers.cend()) {
                ++opt_iter;
                Log.trace("not found in this page");
                continue;
            }
            Log.trace("found in this page");
            // 这种情况可能是需要选择两个同样的技能，上一次循环选了一个，但是没有把滑出当前页面，本次又识别到了这个已选择的人
            if (find_iter->selected == true) {
                if (cur_max_num_of_opers != 1) {
                    cur_all_opers.erase(find_iter);
                    Log.trace("skill matched, but it's selected, pass");
                    continue;
                }
                // 但是如果当前设施只有一个位置，即不存在“上次循环”的情况，说明是清除干员按钮没点到
            }
            else {
                m_ctrler->click(find_iter->rect);
            }
            {
                auto avlb_iter = std::find_if(
                    m_all_available_opers.cbegin(), m_all_available_opers.cend(),
                    [&](const infrast::Oper& lhs) -> bool {
                        int dist = HashImageAnalyzer::hamming(lhs.face_hash, find_iter->face_hash);
                        Log.debug("opers_choose | face hash dist", dist);
                        return dist < face_hash_thres;
                    }
                );
                if (avlb_iter != m_all_available_opers.cend()) {
                    m_all_available_opers.erase(avlb_iter);
                }
                else {
                    Log.error("opers_choose | not found oper");
                }
            }
            ++count;
            cur_all_opers.erase(find_iter);
            opt_iter = m_optimal_combs.erase(opt_iter);
        }
        if (m_optimal_combs.empty()) {
            Log.trace(__FUNCTION__, "| count", count, "cur_max_num_of_opers", cur_max_num_of_opers);
            if (count < cur_max_num_of_opers) {
                // 这种情况可能是萌新，可用干员人数不足以填满当前设施
                callback(AsstMsg::SubTaskExtraInfo, basic_info_with_what("NotEnoughStaff"));
            }
            break;
        }

        // 因为识别完了还要点击，所以这里不能异步滑动
        swipe_of_operlist();
    }

    return true;
}

bool asst::InfrastProductionTask::use_drone()
{
    std::string task_name = "DroneAssist" + facility_name();
    ProcessTask task_temp(*this, { task_name });
    return task_temp.run();
}

asst::infrast::SkillsComb
asst::InfrastProductionTask::efficient_regex_calc(
    std::unordered_set<infrast::Skill> skills) const
{
    infrast::SkillsComb comb(std::move(skills));
    // 根据正则，计算当前干员的实际效率
    for (auto&& [product, formula] : comb.efficient_regex) {
        std::string cur_formula = formula;
        for (size_t pos = 0; pos != std::string::npos;) {
            pos = cur_formula.find('[', pos);
            if (pos == std::string::npos) {
                break;
            }
            size_t rp_pos = cur_formula.find(']', pos);
            if (rp_pos == std::string::npos) {
                break;
                // TODO 报错！
            }
            std::string status_key = cur_formula.substr(pos + 1, rp_pos - pos - 1);
            auto status_opt = m_status->get_number(status_key);
            const int64_t status_value = status_opt ? status_opt.value() : 0;
            cur_formula.replace(pos, rp_pos - pos + 1, std::to_string(status_value));
        }

        int eff = calculator::eval(cur_formula);
        comb.efficient[product] = eff;
    }
    return comb;
}

bool asst::InfrastProductionTask::facility_list_detect()
{
    LogTraceFunction;
    m_facility_list_tabs.clear();

    const auto image = m_ctrler->get_image();
    MultiMatchImageAnalyzer mm_analyzer(image);

    mm_analyzer.set_task_info("InfrastFacilityListTab" + facility_name());

    if (!mm_analyzer.analyze()) {
        return false;
    }
    mm_analyzer.sort_result_horizontal();
    m_facility_list_tabs.reserve(mm_analyzer.get_result().size());
    for (const auto& res : mm_analyzer.get_result()) {
        m_facility_list_tabs.emplace_back(res.rect);
    }

    return true;
}
