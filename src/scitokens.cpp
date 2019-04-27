
#include "XrdAcc/XrdAccAuthorize.hh"
#include "XrdOuc/XrdOucEnv.hh"
#include "XrdSec/XrdSecEntity.hh"
#include "XrdSys/XrdSysLogger.hh"
#include "XrdVersion.hh"

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <sstream>

#include "INIReader.h"
#include "picojson.h"

XrdVERSIONINFO(XrdAccAuthorizeObject, XrdAccSciTokens);

// The status-quo to retrieve the default object is to copy/paste the
// linker definition and invoke directly.
static XrdVERSIONINFODEF(compiledVer, XrdAccTest, XrdVNUMBER, XrdVERSION);
extern XrdAccAuthorize *XrdAccDefaultAuthorizeObject(XrdSysLogger   *lp,
                                                     const char     *cfn,
                                                     const char     *parm,
                                                     XrdVersionInfo &myVer);


namespace {

typedef std::vector<std::pair<Access_Operation, std::string>> AccessRulesRaw;

inline uint64_t monotonic_time() {
  struct timespec tp;
#ifdef CLOCK_MONOTONIC_COARSE
  clock_gettime(CLOCK_MONOTONIC_COARSE, &tp);
#else
  clock_gettime(CLOCK_MONOTONIC, &tp);
#endif
  return tp.tv_sec + (tp.tv_nsec >= 500000000);
}

XrdAccPrivs AddPriv(Access_Operation op, XrdAccPrivs privs)
{
    int new_privs = privs;
    switch (op) {
        case AOP_Any:
            break;
        case AOP_Chmod:
            new_privs |= static_cast<int>(XrdAccPriv_Chmod);
            break;
        case AOP_Chown:
            new_privs |= static_cast<int>(XrdAccPriv_Chown);
            break;
        case AOP_Create:
            new_privs |= static_cast<int>(XrdAccPriv_Create);
            break;
        case AOP_Delete:
            new_privs |= static_cast<int>(XrdAccPriv_Delete);
            break;
        case AOP_Insert:
            new_privs |= static_cast<int>(XrdAccPriv_Insert);
            break;
        case AOP_Lock:
            new_privs |= static_cast<int>(XrdAccPriv_Lock);
            break;
        case AOP_Mkdir:
            new_privs |= static_cast<int>(XrdAccPriv_Mkdir);
            break;
        case AOP_Read:
            new_privs |= static_cast<int>(XrdAccPriv_Read);
            break;
        case AOP_Readdir:
            new_privs |= static_cast<int>(XrdAccPriv_Readdir);
            break;
        case AOP_Rename:
            new_privs |= static_cast<int>(XrdAccPriv_Rename);
            break;
        case AOP_Stat:
            new_privs |= static_cast<int>(XrdAccPriv_Lookup);
            break;
        case AOP_Update:
            new_privs |= static_cast<int>(XrdAccPriv_Update);
            break;
    };
    return static_cast<XrdAccPrivs>(new_privs);
}

}


class XrdAccRules
{
public:
    XrdAccRules(uint64_t expiry_time, const std::string &username) :
        m_expiry_time(expiry_time),
        m_username(username)
    {}

    ~XrdAccRules() {}

    XrdAccPrivs apply(Access_Operation, std::string path) {
        XrdAccPrivs privs = XrdAccPriv_None;
        for (const auto & rule : m_rules) {
            if (!path.compare(0, rule.second.size(), rule.second, 0, rule.second.size())) {
                privs = AddPriv(rule.first, privs);
            }
        }
        return privs;
    }

    bool expired() const {return monotonic_time() > m_expiry_time;}

    void parse(const AccessRulesRaw &rules) {
        m_rules.reserve(rules.size());
        for (const auto &entry : rules) {
            m_rules.emplace_back(entry.first, entry.second);
        }
    }

    const std::string & get_username() const {return m_username;}

private:
    AccessRulesRaw m_rules;
    uint64_t m_expiry_time{0};
    const std::string m_username;
};


class XrdAccSciTokens : public XrdAccAuthorize
{
public:
    XrdAccSciTokens(XrdSysLogger *lp, const char *parms, std::unique_ptr<XrdAccAuthorize> chain) :
        m_chain(std::move(chain)),
        m_parms(parms ? parms : ""),
        m_next_clean(monotonic_time() + m_expiry_secs),
        m_log(lp, "scitokens_")
    {
        m_log.Say("++++++ XrdAccSciTokens: Initialized SciTokens-based authorization.");
        Reconfig();
    }

    virtual ~XrdAccSciTokens() {}

    virtual XrdAccPrivs Access(const XrdSecEntity *Entity,
                                  const char         *path,
                                  const Access_Operation oper,
                                        XrdOucEnv       *env) override
    {
        const char *authz = env ? env->Get("authz") : nullptr;
        if (authz == nullptr) {
            return m_chain ? m_chain->Access(Entity, path, oper, env) : XrdAccPriv_None;
        }
        std::shared_ptr<XrdAccRules> access_rules;
        uint64_t now = monotonic_time();
        Check(now);
        {
            std::lock_guard<std::mutex> guard(m_mutex);
            const auto iter = m_map.find(authz);
            if (iter != m_map.end() && !iter->second->expired()) {
                access_rules = iter->second;
            }
        }
        if (!access_rules) {
            try {
		uint64_t cache_expiry;
		AccessRulesRaw rules;
                std::string username;
                if (GenerateAcls(authz, cache_expiry, rules, username)) {
                    access_rules.reset(new XrdAccRules(now + cache_expiry, username));
                    access_rules->parse(rules);
                } else {
                    return m_chain ? m_chain->Access(Entity, path, oper, env) : XrdAccPriv_None;
                }
            } catch (std::exception &exc) {
                m_log.Emsg("Access", "Error generating ACLs for authorization", exc.what());
                return m_chain ? m_chain->Access(Entity, path, oper, env) : XrdAccPriv_None;
            }
            std::lock_guard<std::mutex> guard(m_mutex);
            m_map[authz] = access_rules;
        }
        const std::string &username = access_rules->get_username();
        if (!username.empty() && !Entity->name) {
            const_cast<XrdSecEntity*>(Entity)->name = strdup(username.c_str());
        }
        XrdAccPrivs result = access_rules->apply(oper, path);
        return ((result == XrdAccPriv_None) && m_chain) ? m_chain->Access(Entity, path, oper, env) : result;
    }

    virtual int Audit(const int              accok,
                      const XrdSecEntity    *Entity,
                      const char            *path,
                      const Access_Operation oper,
                            XrdOucEnv       *Env=0) override
    {
        return 0;
    }

    virtual int         Test(const XrdAccPrivs priv,
                             const Access_Operation oper) override
    {
        return 0;
    }

private:

    bool GenerateAcls(const std::string &authz, uint64_t &cache_expiry, AccessRulesRaw &rules, std::string &username) {
        return false;
    }

    bool Reconfig()
    {
        errno = 0;
        INIReader reader(m_parms);
        if (reader.ParseError() < 0) {
            std::stringstream ss;
            ss << "Error opening config file (" << m_parms << "): " << strerror(errno);
            m_log.Emsg("Reconfig", ss.str().c_str());
            return false;
        } else if (reader.ParseError()) {
            std::stringstream ss;
            ss << "Parse error on line " << reader.ParseError() << " of file " << m_parms;
            m_log.Emsg("Reconfig", ss.str().c_str());
            return false;
        }
        std::vector<std::string> audiences;
        for (const auto &section : reader.Sections()) {
            std::string section_lower;
            std::transform(section.begin(), section.end(), std::back_inserter(section_lower),
                [](unsigned char c){ return std::tolower(c); });

            if (section_lower.substr(0, 6) == "global") {
                auto audience = reader.Get(section, "audience", "");
                if (!audience.empty()) {
                    size_t pos = 0;
                    do {
                        while (audience.size() > pos && (audience[pos] == ',' || audience[pos] == ' ')) {pos++;}
                        auto next_pos = audience.find_first_of(", ", pos);
                        auto next_aud = audience.substr(pos, next_pos - pos);
                        pos = next_pos;
                        if (!next_aud.empty()) {
                            audiences.push_back(next_aud);
                        }
                    } while (pos != std::string::npos);
                }
                audience = reader.Get(section, "audience_json", "");
                if (!audience.empty()) {
                    picojson::value json_obj;
                    auto err = picojson::parse(json_obj, audience);
                    if (!err.empty()) {
                        m_log.Emsg("Reconfig", "Unable to parse audience_json: ", err.c_str());
                        return false;
                    }
                    if (!json_obj.is<picojson::value::array>()) {
                        m_log.Emsg("Reconfig", "audience_json must be a list of strings; not a list.");
                        return false;
                    }
                    for (const auto &val : json_obj.get<picojson::value::array>()) {
                        if (!val.is<std::string>()) {
                            m_log.Emsg("Reconfig", "audience must be a list of strings; value is not a string.");
                            return false;
                        }
                        audiences.push_back(val.get<std::string>());
                    }
                }
            }
        }
        m_audiences = std::move(audiences);
        return true;
    }

    void Check(uint64_t now)
    {
        if (now <= m_next_clean) {return;}

        for (auto iter = m_map.begin(); iter != m_map.end(); iter++) {
            if (iter->second->expired()) {
                m_map.erase(iter);
            }
        }
    }

    std::mutex m_mutex;
    std::vector<std::string> m_audiences;
    std::map<std::string, std::shared_ptr<XrdAccRules>> m_map;
    std::unique_ptr<XrdAccAuthorize> m_chain;
    std::string m_parms;
    uint64_t m_next_clean{0};
    XrdSysError m_log;

    static constexpr uint64_t m_expiry_secs = 60;
};

extern "C" {

XrdAccAuthorize *XrdAccAuthorizeObject(XrdSysLogger *lp,
                                       const char   *cfn,
                                       const char   *parm)
{
    std::unique_ptr<XrdAccAuthorize> def_authz(XrdAccDefaultAuthorizeObject(lp, cfn, parm, compiledVer));
    return new XrdAccSciTokens(lp, parm, std::move(def_authz));
}

}
