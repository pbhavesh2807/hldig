
#include"WordContext.h"
#include"WordType.h"
#include"WordKeyInfo.h"
#include"WordRecord.h"
//#include"WordRecord.h"

WordType       *WordContext::word_type_default=NULL;
WordKeyInfo    *WordContext::key_info         =NULL;
WordRecordInfo *WordContext::record_info      =NULL;
int             WordContext::intialized_ok    =0;
void 
WordContext::Initialize(const Configuration &config)
{
    // Initialize WordType
    WordType::Initialize(config);
    // Initialize WordKeyInfo
    WordKeyInfo::Initialize(config);
    // Initialize WordRecordInfo
    WordRecordInfo::Initialize(config);

    WordContext::intialized_ok    =1;
}

