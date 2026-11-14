#include <Geode/Geode.hpp>
#include <Geode/utils/async.hpp>
#include <Geode/utils/web.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/ui/TextInput.hpp>
#include <Geode/ui/BasedButtonSprite.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <algorithm>
#include <cctype>
#include <deque>
#include <vector>
using namespace geode::prelude;

// ---------- Saved settings ----------
namespace cfg {
    inline int interval() {
        return (int)std::clamp<int64_t>(Mod::get()->getSavedValue<int64_t>("interval", 60), 15, 600);
    }
    inline bool dndOn() {
        return Mod::get()->getSavedValue<bool>("dnd-enabled", false);
    }
    inline int dndPercent() {
        return (int)std::clamp<int64_t>(Mod::get()->getSavedValue<int64_t>("dnd-percent", 50), 1, 100);
    }
    inline std::string sid() {
        return Mod::get()->getSavedValue<std::string>("sid", "");
    }
}

static std::string cleanSid(std::string s) {
    s.erase(std::remove_if(s.begin(), s.end(), [](unsigned char c) {
        return std::isspace(c) || c == '"' || c == '\'';
    }), s.end());
    auto pos = s.find("sid=");
    if (pos != std::string::npos) s = s.substr(pos + 4);
    auto semi = s.find(';');
    if (semi != std::string::npos) s = s.substr(0, semi);
    return s;
}

static void addHeaders(web::WebRequest& req, std::string const& sid) {
    req.header("Cookie", "sid=" + sid);
    req.header("User-Agent", "GachainGD/1.0 (Geode mod)");
}

// ---------- Pretty bottom-right toast ----------
struct ToastInfo {
    std::string title;
    std::string message;
    ccColor3B accent = {120, 200, 255};
    float duration = 5.f;
};

class ToastNode : public CCNode {
    CCNode* m_card = nullptr;
    float m_width = 230.f;
    float m_height = 50.f;
    float m_duration = 5.f;
    bool m_placed = false;

    static constexpr float MARGIN = 10.f;
    static constexpr float GAP = 6.f;
    static constexpr size_t MAX_ACTIVE = 4;

    static inline std::vector<ToastNode*> s_active;
    static inline std::deque<ToastInfo> s_pending;

    bool init(ToastInfo const& info) {
        if (!CCNode::init()) return false;
        m_duration = info.duration;

        std::string text = info.message;
        if (text.size() > 110) text = text.substr(0, 107) + "...";

        float textScale = 0.55f;

        auto title = CCLabelBMFont::create(info.title.c_str(), "bigFont.fnt");
        title->setScale(0.4f);
        title->setColor(info.accent);
        title->setAnchorPoint({0.f, 1.f});

        auto msg = CCLabelBMFont::create(
            text.c_str(), "chatFont.fnt",
            (m_width - 34.f) / textScale, kCCTextAlignmentLeft, CCPointZero
        );
        msg->setScale(textScale);
        msg->setAnchorPoint({0.f, 1.f});

        float titleH = title->getScaledContentSize().height;
        float msgH = msg->getScaledContentSize().height;
        m_height = std::max(44.f, 10.f + titleH + 4.f + msgH + 10.f);

        this->setAnchorPoint({0.f, 0.f});
        this->setContentSize({m_width, m_height});

        // the card slides in and out; the outer node only moves up/down
        m_card = CCNode::create();
        m_card->setAnchorPoint({0.f, 0.f});
        m_card->setContentSize({m_width, m_height});
        m_card->setPosition({m_width + 20.f, 0.f});
        this->addChild(m_card);

        // background (made at 2x and scaled down for smaller rounded corners)
        auto bg = CCScale9Sprite::create("square02b_001.png", {0.f, 0.f, 80.f, 80.f});
        bg->setContentSize({m_width * 2.f, m_height * 2.f});
        bg->setScale(0.5f);
        bg->setAnchorPoint({0.f, 0.f});
        bg->setPosition({0.f, 0.f});
        bg->setColor({18, 20, 28});
        bg->setOpacity(235);
        m_card->addChild(bg);

        // colored accent stripe
        auto bar = CCLayerColor::create(
            ccc4(info.accent.r, info.accent.g, info.accent.b, 255), 3.f, m_height - 18.f
        );
        bar->setPosition({9.f, 9.f});
        m_card->addChild(bar);

        title->setPosition({20.f, m_height - 9.f});
        msg->setPosition({20.f, m_height - 9.f - titleH - 4.f});
        m_card->addChild(title);
        m_card->addChild(msg);

        return true;
    }

    bool present() {
        auto scene = CCDirector::get()->getRunningScene();
        if (!scene) return false;

        auto winSize = CCDirector::get()->getWinSize();
        this->setPositionX(winSize.width - m_width - MARGIN);
        scene->addChild(this, 100000);

        auto slideIn = CCEaseExponentialOut::create(CCMoveTo::create(0.4f, {0.f, 0.f}));
        auto wait = CCDelayTime::create(m_duration);
        auto slideOut = CCEaseExponentialIn::create(CCMoveTo::create(0.3f, {m_width + 20.f, 0.f}));
        auto done = CCCallFunc::create(this, callfunc_selector(ToastNode::finish));
        m_card->runAction(CCSequence::create(slideIn, wait, slideOut, done, nullptr));
        return true;
    }

    // drop toasts whose screen is gone
    static void prune() {
        for (auto it = s_active.begin(); it != s_active.end();) {
            if (!(*it)->getParent()) {
                (*it)->release();
                it = s_active.erase(it);
            } else {
                ++it;
            }
        }
    }

    // stack toasts from the bottom up
    static void layout() {
        float y = MARGIN;
        for (auto& t : s_active) {
            if (!t->m_placed) {
                t->setPositionY(y);
                t->m_placed = true;
            } else {
                t->stopActionByTag(77);
                auto move = CCEaseExponentialOut::create(
                    CCMoveTo::create(0.25f, {t->getPositionX(), y})
                );
                move->setTag(77);
                t->runAction(move);
            }
            y += t->m_height + GAP;
        }
    }

public:
    static ToastNode* create(ToastInfo const& info) {
        auto ret = new ToastNode();
        if (ret->init(info)) { ret->autorelease(); return ret; }
        delete ret;
        return nullptr;
    }

    static void push(ToastInfo info) {
        prune();
        if (s_active.size() >= MAX_ACTIVE) {
            s_pending.push_back(std::move(info));
            return;
        }
        auto node = ToastNode::create(info);
        if (!node) return;
        if (!node->present()) return;
        node->retain();
        s_active.push_back(node);
        layout();
    }

    void finish() {
        this->retain(); // stay alive until we're done in here
        this->removeFromParentAndCleanup(true);
        prune();
        while (!s_pending.empty() && s_active.size() < MAX_ACTIVE) {
            auto next = std::move(s_pending.front());
            s_pending.pop_front();
            push(std::move(next));
        }
        layout();
        this->release();
    }
};

static void toast(std::string title, std::string msg, ccColor3B color) {
    ToastNode::push(ToastInfo{std::move(title), std::move(msg), color, 5.f});
}

// ---------- Do-not-disturb handling ----------
struct Notifier {
    static inline std::vector<ToastInfo> s_held;

    static bool dndActive() {
        if (!cfg::dndOn()) return false;
        auto pl = PlayLayer::get();
        if (!pl) return false;
        if (pl->m_player1 && pl->m_player1->m_isDead) return false;
        return pl->getCurrentPercent() >= (float)cfg::dndPercent();
    }

    static void notify(ToastInfo info) {
        if (dndActive()) {
            if (s_held.size() < 25) s_held.push_back(std::move(info));
            return;
        }
        ToastNode::push(std::move(info));
    }

    static void flushHeld() {
        auto held = std::move(s_held);
        s_held.clear();
        for (auto& i : held) ToastNode::push(std::move(i));
    }
};

static ToastInfo infoFor(std::string const& type, std::string const& msg) {
    ToastInfo i;
    i.message = msg;
    if (type == "trade")       { i.title = "Trade";   i.accent = ccc3(255, 170, 60); }
    else if (type == "badge")  { i.title = "Badge";   i.accent = ccc3(255, 215, 0); }
    else if (type == "friend") { i.title = "Friend";  i.accent = ccc3(90, 170, 255); }
    else if (type == "clan")   { i.title = "Clan";    i.accent = ccc3(180, 120, 255); }
    else if (type == "invite") { i.title = "Invite";  i.accent = ccc3(90, 230, 120); }
    else                       { i.title = "Demon Gacha"; i.accent = ccc3(200, 200, 200); }
    return i;
}

// ---------- Poller ----------
class Poller : public CCNode {
    async::TaskHolder<web::WebResponse> m_notifTask;
    async::TaskHolder<web::WebResponse> m_packTask;
    int64_t m_lastId = 0;
    bool m_firstNotif = true;
    int64_t m_lastTotal = -1;

public:
    static inline Poller* s_instance = nullptr;

    static Poller* create() {
        auto ret = new Poller();
        if (ret->init()) { ret->autorelease(); return ret; }
        delete ret;
        return nullptr;
    }

    bool init() {
        if (!CCNode::init()) return false;
        s_instance = this;
        m_lastId = Mod::get()->getSavedValue<int64_t>("last-id", 0);
        m_firstNotif = (m_lastId == 0);

        this->applyInterval();
        CCDirector::get()->getScheduler()->scheduleSelector(
            schedule_selector(Poller::flushTick), this, 0.5f, false
        );
        this->poll(0);
        return true;
    }

    // (re)start the site check timer using the saved interval
    void applyInterval() {
        auto sched = CCDirector::get()->getScheduler();
        sched->unscheduleSelector(schedule_selector(Poller::poll), this);
        sched->scheduleSelector(
            schedule_selector(Poller::poll), this, (float)cfg::interval(), false
        );
    }

    // call after the sid changes
    void restart() {
        m_lastTotal = -1;
        this->poll(0);
    }

    // releases held notifications once do-not-disturb ends (death, restart, leaving the level)
    void flushTick(float) {
        if (!Notifier::s_held.empty() && !Notifier::dndActive()) {
            Notifier::flushHeld();
        }
    }

    web::WebRequest makeReq() {
        auto req = web::WebRequest();
        addHeaders(req, cfg::sid());
        return req;
    }

    void poll(float) {
        if (cfg::sid().empty()) return;

        m_notifTask.spawn(
            makeReq().get("https://demongacha.com/api/notifications"),
            [this](web::WebResponse res) { this->onNotifs(res); }
        );
        m_packTask.spawn(
            makeReq().get("https://demongacha.com/api/packs"),
            [this](web::WebResponse res) { this->onPacks(res); }
        );
    }

    void onPacks(web::WebResponse& res) {
        if (!res.ok()) return;
        auto json = res.json();
        if (!json) return;

        auto obj = json.unwrap();
        int64_t total = obj["packs"].asInt().unwrapOr(0) + obj["bonus"].asInt().unwrapOr(0);

        if (m_lastTotal >= 0 && total > m_lastTotal) {
            ToastInfo info;
            info.title = "New pack ready!";
            info.message = fmt::format("You now have {} pack{}.", total, total == 1 ? "" : "s");
            info.accent = ccc3(90, 230, 120);
            Notifier::notify(std::move(info));
        }
        m_lastTotal = total;
    }

    void onNotifs(web::WebResponse& res) {
        if (!res.ok()) return;
        auto json = res.json();
        if (!json) return;
        auto arr = json.unwrap().asArray();
        if (!arr) return;

        int64_t newest = m_lastId;
        std::vector<ToastInfo> fresh; // newest first, like the site's list

        for (auto& item : arr.unwrap()) {
            int64_t id = item["id"].asInt().unwrapOr(0);
            if (id > newest) newest = id;
            if (!m_firstNotif && id > m_lastId) {
                auto type = item["type"].asString().unwrapOr("");
                auto msg = item["message"].asString().unwrapOr("New notification");
                fresh.push_back(infoFor(type, msg));
            }
        }

        std::reverse(fresh.begin(), fresh.end()); // show oldest first
        if (fresh.size() > 4) {
            size_t extra = fresh.size() - 3;
            fresh.resize(3);
            ToastInfo more;
            more.title = "Demon Gacha";
            more.message = fmt::format("...and {} more notifications.", extra);
            fresh.push_back(std::move(more));
        }
        for (auto& i : fresh) Notifier::notify(std::move(i));

        m_firstNotif = false;
        m_lastId = newest;
        Mod::get()->setSavedValue("last-id", newest);
    }
};

// ---------- Settings popup ----------
class SidPopup : public Popup {
    TextInput* m_sidInput = nullptr;
    TextInput* m_intervalInput = nullptr;
    TextInput* m_percentInput = nullptr;
    CCMenuItemToggler* m_dndToggle = nullptr;
    async::TaskHolder<web::WebResponse> m_testTask;
    bool m_testing = false;

    CCLabelBMFont* addLabel(char const* text, CCPoint pos, float scale, char const* font = "bigFont.fnt") {
        auto l = CCLabelBMFont::create(text, font);
        l->setScale(scale);
        l->setAnchorPoint({0.f, 0.5f});
        l->setPosition(pos);
        m_mainLayer->addChild(l);
        return l;
    }

protected:
    bool init() {
        if (!Popup::init(360.f, 270.f)) return false;

        this->setTitle("Demon Gacha");
        auto size = m_mainLayer->getContentSize();

        // --- sid ---
        addLabel("Login cookie (sid)", {20.f, 208.f}, 0.4f);
        m_sidInput = TextInput::create(320.f, "Paste sid (empty = keep current)", "bigFont.fnt");
        m_sidInput->setPasswordMode(true);
        m_sidInput->setPosition({size.width / 2, 182.f});
        m_mainLayer->addChild(m_sidInput);

        // --- interval ---
        addLabel("Check every", {20.f, 140.f}, 0.4f);
        m_intervalInput = TextInput::create(70.f, "60", "bigFont.fnt");
        m_intervalInput->setFilter("0123456789");
        m_intervalInput->setMaxCharCount(3);
        m_intervalInput->setString(std::to_string(cfg::interval()));
        m_intervalInput->setPosition({290.f, 140.f});
        m_mainLayer->addChild(m_intervalInput);
        addLabel("sec", {328.f, 140.f}, 0.35f);

        // --- do not disturb ---
        m_dndToggle = CCMenuItemToggler::createWithStandardSprites(
            this, menu_selector(SidPopup::onNoop), 0.6f
        );
        m_dndToggle->toggle(cfg::dndOn());
        m_dndToggle->setPosition({28.f, 100.f});
        m_buttonMenu->addChild(m_dndToggle);

        addLabel("Do not disturb in levels at", {46.f, 100.f}, 0.3f);
        m_percentInput = TextInput::create(56.f, "50", "bigFont.fnt");
        m_percentInput->setFilter("0123456789");
        m_percentInput->setMaxCharCount(3);
        m_percentInput->setString(std::to_string(cfg::dndPercent()));
        m_percentInput->setPosition({290.f, 100.f});
        m_mainLayer->addChild(m_percentInput);
        addLabel("%", {324.f, 100.f}, 0.4f);

        auto hint = addLabel("Alerts wait until you die or leave the level", {46.f, 74.f}, 0.5f, "chatFont.fnt");
        hint->setColor(ccc3(170, 170, 170));

        // --- buttons ---
        auto pasteBtn = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Paste"), this, menu_selector(SidPopup::onPaste)
        );
        auto saveBtn = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Save"), this, menu_selector(SidPopup::onSave)
        );
        pasteBtn->setPosition({size.width / 2 - 60.f, 36.f});
        saveBtn->setPosition({size.width / 2 + 60.f, 36.f});
        m_buttonMenu->addChild(pasteBtn);
        m_buttonMenu->addChild(saveBtn);
        return true;
    }

    void onNoop(CCObject*) {}

    void onPaste(CCObject*) {
        m_sidInput->setString(clipboard::read());
    }

    void onSave(CCObject*) {
        if (m_testing) return;

        // save the simple settings first
        int interval = std::clamp(
            utils::numFromString<int>(m_intervalInput->getString()).unwrapOr(cfg::interval()), 15, 600
        );
        int percent = std::clamp(
            utils::numFromString<int>(m_percentInput->getString()).unwrapOr(cfg::dndPercent()), 1, 100
        );
        Mod::get()->setSavedValue<int64_t>("interval", (int64_t)interval);
        Mod::get()->setSavedValue<int64_t>("dnd-percent", (int64_t)percent);
        Mod::get()->setSavedValue<bool>("dnd-enabled", m_dndToggle->isToggled());
        if (Poller::s_instance) Poller::s_instance->applyInterval();

        std::string sid = cleanSid(m_sidInput->getString());

        // no new sid typed: just keep the old one
        if (sid.empty()) {
            if (cfg::sid().empty()) {
                FLAlertLayer::create("Oops", "Paste your sid first.", "OK")->show();
                return;
            }
            toast("Settings saved", fmt::format("Checking every {} seconds.", interval), ccc3(90, 230, 120));
            this->onClose(nullptr);
            return;
        }

        // test the new sid before keeping it
        m_testing = true;
        toast("Testing connection...", "Contacting demongacha.com", ccc3(120, 200, 255));

        auto req = web::WebRequest();
        addHeaders(req, sid);

        m_testTask.spawn(
            req.get("https://demongacha.com/api/packs"),
            [this, sid, interval](web::WebResponse res) {
                m_testing = false;
                int code = res.code();

                if (res.ok()) {
                    int64_t packs = -1;
                    if (auto json = res.json()) {
                        packs = json.unwrap()["packs"].asInt().unwrapOr(-1);
                    }

                    Mod::get()->setSavedValue("sid", sid);
                    if (Poller::s_instance) Poller::s_instance->restart();

                    std::string msg = packs >= 0
                        ? fmt::format("You have {} packs. Checking every {}s.", packs, interval)
                        : fmt::format("Checking every {} seconds.", interval);
                    toast("Connected!", msg, ccc3(90, 230, 120));
                    this->onClose(nullptr);
                }
                else if (code == 401 || code == 403) {
                    std::string tail = sid.size() >= 3 ? sid.substr(sid.size() - 3) : sid;
                    toast("Sid rejected",
                        fmt::format("Code {}, length {}, {}...{}", code, sid.size(), sid.substr(0, 3), tail),
                        ccc3(255, 90, 90));
                    log::warn("Server said: {}", res.string().unwrapOr(""));
                }
                else if (code == 0) {
                    toast("No connection", "Couldn't reach the server. Check your internet.", ccc3(255, 90, 90));
                }
                else {
                    toast("Server error", fmt::format("The server returned code {}.", code), ccc3(255, 90, 90));
                }
            }
        );
    }

public:
    static SidPopup* create() {
        auto ret = new SidPopup();
        if (ret->init()) { ret->autorelease(); return ret; }
        delete ret;
        return nullptr;
    }
};

// ---------- Main menu: only starts the background checker ----------
class $modify(MyMenuLayer, MenuLayer) {
    bool init() {
        if (!MenuLayer::init()) return false;

        static bool started = false;
        if (!started) {
            started = true;
            auto poller = Poller::create();
            if (poller) poller->retain(); // keep alive for the whole session
        }
        return true;
    }
};

// ---------- Pause menu: the DG button ----------
class $modify(MyPauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();

        auto menu = this->getChildByID("left-button-menu");
        if (!menu) return;

        auto label = CCLabelBMFont::create("DG", "bigFont.fnt");
        auto spr = CircleButtonSprite::create(label);
        auto btn = CCMenuItemSpriteExtra::create(
            spr, this, menu_selector(MyPauseLayer::onDemonGacha)
        );
        btn->setID("demongacha-button"_spr);
        menu->addChild(btn);
        menu->updateLayout();
    }

    void onDemonGacha(CCObject*) {
        SidPopup::create()->show();
    }
};
