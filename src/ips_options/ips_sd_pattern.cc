//--------------------------------------------------------------------------
// Copyright (C) 2016-2016 Cisco and/or its affiliates. All rights reserved.
//
// This program is free software; you can redistribute it and/or modify it
// under the terms of the GNU General Public License Version 2 as published
// by the Free Software Foundation.  You may not use, modify or distribute
// this program under any other version of the GNU General Public License.
//
// This program is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
//--------------------------------------------------------------------------

// ips_sd_pattern.cc author Victor Roemer <viroemer@cisco.com>

#include "ips_sd_pattern.h"

#include <string.h>
#include <assert.h>
#include <string>

#include <hs_compile.h>
#include <hs_runtime.h>

#include "framework/cursor.h"
#include "framework/ips_option.h"
#include "framework/module.h"
#include "detection/detection_defines.h"
#include "detection/pattern_match_data.h"
#include "hash/sfhashfcn.h"
#include "main/snort_config.h"
#include "main/thread.h"
#include "parser/parser.h"
#include "profiler/profiler.h"
#include "sd_credit_card.h"
#include "log/obfuscator.h"

#define s_name "sd_pattern"
#define s_help "rule option for detecting sensitive data"

#define SD_SOCIAL_PATTERN          "\\b\\d{3}-\\d{2}-\\d{4}\\b"
#define SD_SOCIAL_NODASHES_PATTERN "\\b\\d{9}\\b"
#define SD_CREDIT_PATTERN_ALL      "\\b\\d{4}[- ]?\\d{4}[- ]?\\d{2}[- ]?\\d{2}[- ]?\\d{3,4}\\b"

// we need to update scratch in the main thread as each pattern is processed
// and then clone to thread specific after all rules are loaded.  s_scratch is
// a prototype that is large enough for all uses.

// FIXIT-L Determine if it's worthwhile to use a single scratch space for both
// "regex" and "sd_pattern" keywords.
// FIXIT-L See ips_regex.cc for more information.
static hs_scratch_t* s_scratch = nullptr;

struct SdStats
{
    PegCount nomatch_threshold;
    PegCount nomatch_notfound;
    PegCount terminated;
};

const PegInfo sd_pegs[] =
{
    { "below threshold", "sd_pattern matched but missed threshold" },
    { "pattern not found", "sd_pattern did not not match" },
    { "terminated", "hyperscan terminated" },
    { nullptr, nullptr }
};

static THREAD_LOCAL SdStats s_stats;

struct SdPatternConfig
{
    hs_database_t* db;

    std::string pii;
    unsigned threshold = 1;
    bool obfuscate_pii = false;
    int (*validate)(const uint8_t* buf, unsigned long long buflen) = nullptr;

    inline bool operator==(const SdPatternConfig& rhs) const
    {
        if ( pii == rhs.pii and threshold == rhs.threshold )
            return true;
        return false;
    }
};

static THREAD_LOCAL ProfileStats sd_pattern_perf_stats;

//-------------------------------------------------------------------------
// option
//-------------------------------------------------------------------------

class SdPatternOption : public IpsOption
{
public:
    SdPatternOption(const SdPatternConfig&);
    ~SdPatternOption();

    uint32_t hash() const override;
    bool operator==(const IpsOption&) const override;

    int eval(Cursor&, Packet* p) override;

private:
    unsigned SdSearch(Cursor&, Packet*);
    const SdPatternConfig config;
};

SdPatternOption::SdPatternOption(const SdPatternConfig& c) :
    IpsOption(s_name, RULE_OPTION_TYPE_BUFFER_USE), config(c)
{
    if ( hs_error_t err = hs_alloc_scratch(config.db, &s_scratch) )
    {
        // FIXIT-L why is this failing but everything is working?
        ParseError("can't initialize sd_pattern for %s (%d) %p",
                config.pii.c_str(), err, (void*)s_scratch);
    }
}

SdPatternOption::~SdPatternOption()
{ 
    if ( config.db )
        hs_free_database(config.db);
}

uint32_t SdPatternOption::hash() const
{
    uint32_t a = 0, b = 0, c = config.threshold;
    mix_str(a, b, c, config.pii.c_str());
    mix_str(a, b, c, get_name());
    finalize(a, b, c);
    return c;
}

bool SdPatternOption::operator==(const IpsOption& ips) const
{
    if ( !IpsOption::operator==(ips) )
        return false;

    const SdPatternOption& rhs = static_cast<const SdPatternOption&>(ips);

    if ( config == rhs.config )
        return true;

    return false;
}

struct hsContext 
{
    hsContext(const SdPatternConfig &c_, Packet* p_, const uint8_t* const start_)
        : config(c_), packet(p_), start(start_) {}

    unsigned int count = 0;

    SdPatternConfig config;
    Packet* packet = nullptr;
    const uint8_t* const start = nullptr;
    const uint8_t* buf = nullptr;
};

// FIXIT-H Count matches
// FIXIT-H afix this to SdPatternOption
int hs_match(unsigned int /*id*/, unsigned long long from,
        unsigned long long to, unsigned int /*flags*/, void *context)
{
    hsContext* ctx = (hsContext*) context;

    assert(ctx);
    assert(ctx->packet);
    assert(ctx->start);

    unsigned long long len = to - from;
    if ( ctx->config.validate && ctx->config.validate(ctx->buf, len) != 1 )
        return 0;

    ctx->count++;

    if ( ctx->config.obfuscate_pii )
    {
        if ( !ctx->packet->obfuscator )
            ctx->packet->obfuscator = new Obfuscator();

        uint32_t off = ctx->buf - ctx->start;
        // FIXIT-L Make configurable or don't show any PII partials (0 for user defined??) 
        len = len > 4 ? len - 4 : len;
        ctx->packet->obfuscator->push(off, len);
    }

    return 0;
}

unsigned SdPatternOption::SdSearch(Cursor& c, Packet* p)
{
    const uint8_t* const start = c.buffer();
    const uint8_t* buf = c.start();
    unsigned int buflen = c.length();

    SnortState* ss = snort_conf->state + get_instance_id();
    assert(ss->sdpattern_scratch);

    hsContext ctx(config, p, start);
    ctx.buf = buf;

    hs_error_t stat = hs_scan(config.db, (const char*)buf, buflen, 0,
        (hs_scratch_t*)ss->sdpattern_scratch, hs_match, (void*)&ctx);

    if ( stat == HS_SCAN_TERMINATED )
        ++s_stats.terminated;

    return ctx.count;
}

int SdPatternOption::eval(Cursor& c, Packet* p)
{
    Profile profile(sd_pattern_perf_stats);

    unsigned matches = SdSearch(c, p);

    if ( matches >= config.threshold )
        return DETECTION_OPTION_MATCH;
    else if ( matches == 0 )
        ++s_stats.nomatch_notfound;
    else if ( matches > 0 && matches < config.threshold )
        ++s_stats.nomatch_threshold;

    return DETECTION_OPTION_NO_MATCH;
}

//-------------------------------------------------------------------------
// module
//-------------------------------------------------------------------------

static const Parameter s_params[] =
{
    { "~pattern", Parameter::PT_STRING, nullptr, nullptr,
      "The pattern to search for" },

    { "threshold", Parameter::PT_INT, "1", nullptr,
      "number of matches before alerting" },

    { nullptr, Parameter::PT_MAX, nullptr, nullptr, nullptr }
};

class SdPatternModule : public Module
{
public:
    SdPatternModule() : Module(s_name, s_help, s_params) { }

    bool begin(const char*, int, SnortConfig*) override;
    bool set(const char*, Value& v, SnortConfig*) override;
    bool end(const char*, int, SnortConfig*) override;

    const PegInfo* get_pegs() const override
    { return sd_pegs; }

    PegCount* get_counts() const override
    { return (PegCount*)&s_stats; }

    ProfileStats* get_profile() const override
    { return &sd_pattern_perf_stats; }

    void get_data(SdPatternConfig& c)
    { c = config; }

private:
    SdPatternConfig config;
};

bool SdPatternModule::begin(const char*, int, SnortConfig*)
{
    config = SdPatternConfig();
    return true;
}

bool SdPatternModule::set(const char*, Value& v, SnortConfig* sc)
{
    config.obfuscate_pii = false;

    if ( v.is("~pattern") )
    {
        config.pii = v.get_string();
        // remove quotes
        config.pii.erase(0, 1);
        config.pii.erase(config.pii.length()-1, 1);
    }
    else if ( v.is("threshold") )
        config.threshold = v.get_long();
    else
        return false;

    // Check if built-in pattern should be used.
    if (config.pii == "credit_card")
    {
        config.pii = SD_CREDIT_PATTERN_ALL;
        config.validate = SdLuhnAlgorithm;
        config.obfuscate_pii = sc->obfuscate_pii;
    }

    else if (config.pii == "us_social")
    {
        config.pii = SD_SOCIAL_PATTERN;
        config.obfuscate_pii = sc->obfuscate_pii;
    }

    else if (config.pii == "us_social_nodashes")
    {
        config.pii = SD_SOCIAL_NODASHES_PATTERN;
        config.obfuscate_pii = sc->obfuscate_pii;
    }

    return true;
}

bool SdPatternModule::end(const char*, int, SnortConfig*)
{
    hs_compile_error_t* err = nullptr;

    if ( hs_compile(config.pii.c_str(), HS_FLAG_DOTALL|HS_FLAG_SOM_LEFTMOST, HS_MODE_BLOCK, nullptr, &config.db, &err)
        or !config.db )
    {
        ParseError("can't compile regex '%s'", config.pii.c_str());
        hs_free_compile_error(err);
        return false;
    }
    return true;
}

//-------------------------------------------------------------------------
// public methods
//-------------------------------------------------------------------------

void sdpattern_setup(SnortConfig* sc)
{
    for ( unsigned i = 0; i < sc->num_slots; ++i )
    {
        SnortState* ss = sc->state + i;

        if ( s_scratch )
            hs_clone_scratch(s_scratch, (hs_scratch_t**)&ss->sdpattern_scratch);
        else
            ss->sdpattern_scratch = nullptr;
    }
}

void sdpattern_cleanup(SnortConfig* sc)
{
    for ( unsigned i = 0; i < sc->num_slots; ++i )
    {
        SnortState* ss = sc->state + i;

        if ( ss->sdpattern_scratch )
        {
            hs_free_scratch((hs_scratch_t*)ss->sdpattern_scratch);
            ss->sdpattern_scratch = nullptr;
        }
    }
}

//-------------------------------------------------------------------------
// api methods
//-------------------------------------------------------------------------

static Module* mod_ctor()
{
    return new SdPatternModule;
}

static void mod_dtor(Module* p)
{
    delete p;
}

static IpsOption* sd_pattern_ctor(Module* m, OptTreeNode*)
{
    SdPatternModule* mod = (SdPatternModule*)m;
    SdPatternConfig c;
    mod->get_data(c);
    return new SdPatternOption(c);
}

static void sd_pattern_dtor(IpsOption* p)
{ 
    delete p;
}

static const IpsApi sd_pattern_api =
{
    {
        PT_IPS_OPTION,
        sizeof(IpsApi),
        IPSAPI_VERSION,
        0,
        API_RESERVED,
        API_OPTIONS,
        s_name,
        s_help,
        mod_ctor,
        mod_dtor
    },
    OPT_TYPE_DETECTION,
    0, 0,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    sd_pattern_ctor,
    sd_pattern_dtor,
    nullptr
};

#ifdef BUILDING_SO
SO_PUBLIC const BaseApi* snort_plugins[] =
{
    &sd_pattern_api.base,
    nullptr
};
#else
const BaseApi* ips_sd_pattern = &sd_pattern_api.base;
#endif

