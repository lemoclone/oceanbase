/**
 * (C) 2010-2012 Alibaba Group Holding Limited.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * Version: $Id$
 *
 * ob_root_bootstrap.cpp
 *
 * Authors:
 *   Zhifeng YANG <zhuweng.yzf@taobao.com>
 *   zhidong sun <xielun.szd@alipay.com>
 *
 */
#include <stdio.h>
#include "ob_root_bootstrap.h"
#include "ob_root_server2.h"
#include "common/ob_encrypted_helper.h"
#include "common/nb_accessor/ob_nb_accessor.h"
#include "common/ob_schema_service_impl.h"
#include "common/ob_schema_helper.h"
#include "common/ob_extra_tables_schema.h"
#include "common/ob_inner_table_operator.h"
#include "common/ob_version.h"
#include "common/ob_trigger_event_util.h"
#include "common/ob_config_manager.h"

using namespace oceanbase::rootserver;
using namespace oceanbase::common;

ObBootstrap::ObBootstrap(ObRootServer2 & root_server):root_server_(root_server), log_worker_(NULL)
{
  core_table_count_ = 0;
  sys_table_count_ = 0;
  ini_table_count_ = 0;
}

ObBootstrap::~ObBootstrap()
{
}

int64_t ObBootstrap::get_table_count(void) const
{
  return core_table_count_ + sys_table_count_ + ini_table_count_;
}

void ObBootstrap::set_log_worker(ObRootLogWorker * log_worker)
{
  log_worker_ = log_worker;
}

// create inner core table
int ObBootstrap::create_core_table(const uint64_t table_id, ObServerArray & first_tablet_entry_cs)
{
  TableSchema table_schema;
  int ret = root_server_.get_table_schema(table_id, table_schema);
  if (ret != OB_SUCCESS)
  {
    TBSYS_LOG(WARN, "fail to get talbe schema. table_id=%lu err=%d", table_id, ret);
  }
  else
  {
    ret = root_server_.create_empty_tablet(table_schema, first_tablet_entry_cs);
    if (OB_SUCCESS != ret)
    {
      TBSYS_LOG(WARN, "fail to create empty tablet. table_id=%lu err=%d", table_id, ret);
    }
    else
    {
      ++core_table_count_;
      TBSYS_LOG(INFO, "create tablet success. table_id=%lu", table_id);
    }
  }
  return ret;
}

int ObBootstrap::create_all_core_tables()
{
  //1. create first_tablet_entry tablet
  ObServerArray first_tablet_entry_cs;
  int ret = create_core_table(OB_FIRST_TABLET_ENTRY_TID, first_tablet_entry_cs);
  if (OB_SUCCESS != ret)
  {
    TBSYS_LOG(WARN, "fail to create first_tablet_entry's tablet. err=%d", ret);
  }
  else
  {
    TBSYS_LOG(INFO, "create first_tablet_entry tablet success. ");
  }
  //2.create all_all_column tablet
  if (OB_SUCCESS == ret)
  {
    ObServerArray cs;
    ret = create_core_table(OB_ALL_ALL_COLUMN_TID, cs);
    if (OB_SUCCESS != ret)
    {
      TBSYS_LOG(WARN, "fail to create all_all_column_id's tablet. err=%d", ret);
    }
    else
    {
      TBSYS_LOG(INFO, "create all_all_column_id tablet success.");
    }
  }
  //3.create all_all_join_info tablet
  if (OB_SUCCESS == ret)
  {
    ObServerArray cs;
    ret = create_core_table(OB_ALL_JOIN_INFO_TID, cs);
    if (ret != OB_SUCCESS)
    {
      TBSYS_LOG(WARN, "fail to create all_all_join_info's tablet. err=%d", ret);
    }
    else
    {
      TBSYS_LOG(INFO, "create all_all_join_info tablet success.");
    }
  }
  //4.init meta file
  if (OB_SUCCESS == ret)
  {
    if (OB_SUCCESS != (ret = init_meta_file(first_tablet_entry_cs)))
    {
      TBSYS_LOG(WARN, "fail to init meta file. err=%d", ret);
    }
    else
    {
      TBSYS_LOG(INFO, "init meta file success.");
    }
  }
  return ret;
}

int ObBootstrap::bootstrap_core_tables(void)
{
  int ret = init_schema_service();
  if (ret != OB_SUCCESS)
  {
    TBSYS_LOG(WARN, "init schema service failed:ret[%d]", ret);
  }
  else
  {
    // create first 3 core tables
    ret = create_all_core_tables();
    if (ret != OB_SUCCESS)
    {
      TBSYS_LOG(ERROR, "create all core tables failed:ret[%d]", ret);
    }
    else
    {
      TBSYS_LOG(INFO, "create oceanbase core tables succ");
    }
  }
  return ret;
}

//
int ObBootstrap::create_sys_table(TableSchema & table_schema)
{
  int ret = OB_SUCCESS;
  const ObiRole &role = root_server_.get_obi_role();
  if (table_schema.table_id_ >= OB_APP_MIN_TABLE_ID)
  {
    ret = OB_INVALID_ARGUMENT;
    TBSYS_LOG(WARN, "check sys table id failed:table_name=%s, table_id=%lu, "
        "min_app_id=%lu", table_schema.table_name_, table_schema.table_id_,
        OB_APP_MIN_TABLE_ID);
  }
  else if (ObiRole::MASTER == role.get_role())
  {
    ret = schema_service_.create_table(table_schema);
    if (OB_SUCCESS != ret)
    {
      TBSYS_LOG(WARN, "failed to create from schema service. table_id=%lu, err=%d",
          table_schema.table_id_, ret);
    }
  }
  if (OB_SUCCESS == ret)
  {
    common::ObArray<common::ObServer> cs;
    ret = root_server_.create_empty_tablet(table_schema, cs);
    if (OB_SUCCESS != ret)
    {
      TBSYS_LOG(WARN, "failed to create empty tablet for table_id=%lu, err=%d",
          table_schema.table_id_, ret);
    }
    else if (cs.count() > 0)
    {
      ++sys_table_count_;
      TBSYS_LOG(INFO, "created sys table succ, table_name=%s table_id=%lu cs_num=%ld",
          table_schema.table_name_, table_schema.table_id_, cs.count());
    }
    else
    {
      ret = OB_ERROR;
      TBSYS_LOG(WARN, "create table succ but check cs count is zero. table_id=%lu",
          table_schema.table_id_);
    }
  }
  return ret;
}

int ObBootstrap::init_schema_service(void)
{
  int ret = schema_service_.init(root_server_.schema_service_scan_helper_, true);
  if (OB_SUCCESS != ret)
  {
    TBSYS_LOG(WARN, "failed to init schema service, err=%d", ret);
  }
  return ret;
}

//create schema.ini table
int ObBootstrap::bootstrap_ini_tables()
{
  int ret = OB_SUCCESS;
  common::ObSchemaManagerV2 *schema_manager = NULL;
  const ObiRole &role = root_server_.get_obi_role();
  if (NULL == (schema_manager = root_server_.get_ini_schema()))
  {
    ret = OB_ERROR;
    TBSYS_LOG(WARN, "ini schema_manager is NULL.");
  }
  else
  {
    ObArray<TableSchema> table_array;
    if (OB_SUCCESS != (ret = ObSchemaHelper::transfer_manager_to_table_schema(*schema_manager, table_array)))
    {
      TBSYS_LOG(WARN, "fail to transfer manager to table schema. err=%d", ret);
    }
    else
    {
      TBSYS_LOG(DEBUG, "check ini table count:array[%ld], schema[%ld]", table_array.count(),
          schema_manager->get_table_count());
      for (int64_t i = 0; i < table_array.count(); i++)
      {
        // TODO check the schema.ini table id between (1000, 3000)
        if (role.get_role() == ObiRole::MASTER)
        {
          if (OB_SUCCESS != (ret = schema_service_.create_table(table_array.at(i))))
          {
            TBSYS_LOG(ERROR, "fail to create table. table_name=%s", table_array.at(i).table_name_);
            break;
          }
        }
        if (OB_SUCCESS == ret)
        {
          ++ini_table_count_;
          TBSYS_LOG(INFO, "create ini table success. table_name=%s", table_array.at(i).table_name_);
        }
      }
    }
  }
  return ret;
}

int ObBootstrap::bootstrap_sys_tables(void)
{
  // create table __all_sys_stat
  TableSchema table_schema;
  int ret = ObExtraTablesSchema::all_sys_stat_schema(table_schema);
  if (OB_SUCCESS != ret)
  {
    TBSYS_LOG(WARN, "failed to get schema for all_sys_stat, err=%d", ret);
  }
  else if (OB_SUCCESS != (ret = create_sys_table(table_schema)))
  {
    TBSYS_LOG(WARN, "failed to create empty tablet for __all_sys_stat, err=%d", ret);
  }
  // create table __all_sys_param
  if (OB_SUCCESS == ret)
  {
    if (OB_SUCCESS != (ret = ObExtraTablesSchema::all_sys_param_schema(table_schema)))
    {
      TBSYS_LOG(WARN, "failed to get schema for all_sys_param, err=%d", ret);
    }
    else if (OB_SUCCESS != (ret = create_sys_table(table_schema)))
    {
      TBSYS_LOG(WARN, "failed to create empty tablet for __all_sys_param, err=%d", ret);
    }
  }
  // create table __all_sys_config
  if (OB_SUCCESS == ret)
  {
    if (OB_SUCCESS != (ret = ObExtraTablesSchema::all_sys_config_schema(table_schema)))
    {
      TBSYS_LOG(WARN, "failed to get schema for all_sys_param, err=%d", ret);
    }
    else if (OB_SUCCESS != (ret = create_sys_table(table_schema)))
    {
      TBSYS_LOG(WARN, "failed to create empty tablet for __all_sys_param, err=%d", ret);
    }
  }
  // create talbe __trigger_event
  if (OB_SUCCESS == ret)
  {
    if (OB_SUCCESS != (ret = ObExtraTablesSchema::all_trigger_event_schema(table_schema)))
    {
      TBSYS_LOG(WARN, "failed to get schema for __all_trigger_event, err=%d", ret);
    }
    else if (OB_SUCCESS != (ret = create_sys_table(table_schema)))
    {
      TBSYS_LOG(WARN, "failed to create empty tablet for __all_trigger_event, err=%d", ret);
    }
  }
  // create table __users
  if (OB_SUCCESS == ret)
  {
    if (OB_SUCCESS != (ret = ObExtraTablesSchema::all_user_schema(table_schema)))
    {
      TBSYS_LOG(WARN, "failed to get schema for __all_user, err=%d", ret);
    }
    else if (OB_SUCCESS != (ret = create_sys_table(table_schema)))
    {
      TBSYS_LOG(WARN, "failed to create empty tablet for __all_user, err=%d", ret);
    }
  }
  // create table __table_privileges
  if (OB_SUCCESS == ret)
  {
    if (OB_SUCCESS != (ret = ObExtraTablesSchema::all_table_privilege_schema(table_schema)))
    {
      TBSYS_LOG(WARN, "failed to get schema for __all_table_privilege, err=%d", ret);
    }
    else if (OB_SUCCESS != (ret = create_sys_table(table_schema)))
    {
      TBSYS_LOG(WARN, "failed to create empty tablet for __all_table_privilege, err=%d", ret);
    }
  }
  // create table __all_cluster
  if (OB_SUCCESS == ret)
  {
    if (OB_SUCCESS != (ret = ObExtraTablesSchema::all_cluster_schema(table_schema)))
    {
      TBSYS_LOG(WARN, "failed to get schema of __all_cluster, err=%d", ret);
    }
    else if (OB_SUCCESS != (ret = create_sys_table(table_schema)))
    {
      TBSYS_LOG(WARN, "failed to create table for __all_cluster, err=%d", ret);
    }
  }
  // create table __all_server
  if (OB_SUCCESS == ret)
  {
    if (OB_SUCCESS != (ret = ObExtraTablesSchema::all_server_schema(table_schema)))
    {
      TBSYS_LOG(WARN, "failed to get schema of __all_server, err=%d", ret);
    }
    else if (OB_SUCCESS != (ret = create_sys_table(table_schema)))
    {
      TBSYS_LOG(WARN, "failed to create table for __all_server, err=%d", ret);
    }
  }
  // create table __all_client
  if (OB_SUCCESS == ret)
  {
    if (OB_SUCCESS != (ret = ObExtraTablesSchema::all_client_schema(table_schema)))
    {
      TBSYS_LOG(WARN, "failed to get schema of __all_client, err=%d", ret);
    }
    else if (OB_SUCCESS != (ret = create_sys_table(table_schema)))
    {
      TBSYS_LOG(WARN, "failed to create table for __all_client, err=%d", ret);
    }
  }
  // create table __all_server_stat
  if (OB_SUCCESS == ret)
  {
    if (OB_SUCCESS != (ret = ObExtraTablesSchema::all_server_stat_schema(table_schema)))
    {
      TBSYS_LOG(WARN, "failed to get schema of __all_server_stat, err=%d", ret);
    }
    else if (OB_SUCCESS != (ret = create_sys_table(table_schema)))
    {
      TBSYS_LOG(WARN, "failed to create table for __all_server_stat, err=%d", ret);
    }
  }

  // create table __all_server_session
  if (OB_SUCCESS == ret)
  {
    if (OB_SUCCESS != (ret = ObExtraTablesSchema::all_server_session_schema(table_schema)))
    {
      TBSYS_LOG(WARN, "failed to get schema of __all_server_session, err=%d", ret);
    }
    else if (OB_SUCCESS != (ret = create_sys_table(table_schema)))
    {
      TBSYS_LOG(WARN, "failed to create table for __all_server_session, err=%d", ret);
    }
  }
  // create table __all_sys_config_stat
  if (OB_SUCCESS == ret)
  {
    if (OB_SUCCESS != (ret = ObExtraTablesSchema::all_sys_config_stat_schema(table_schema)))
    {
      TBSYS_LOG(WARN, "failed to get schema for all_sys_config_stat, err=%d", ret);
    }
    else if (OB_SUCCESS != (ret = create_sys_table(table_schema)))
    {
      TBSYS_LOG(WARN, "failed to create empty tablet for __all_sys_config_stat, err=%d", ret);
    }
  }
  // create table __all_statement
  if (OB_SUCCESS == ret)
  {
    if (OB_SUCCESS != (ret = ObExtraTablesSchema::all_statement_schema(table_schema)))
    {
      TBSYS_LOG(WARN, "failed to get schema of __all_statement, err=%d", ret);
    }
    else if (OB_SUCCESS != (ret = create_sys_table(table_schema)))
    {
      TBSYS_LOG(WARN, "failed to create table for __all_statement, err=%d", ret);
    }
  }
  return ret;
}

int ObBootstrap::init_system_table()
{
  int ret = OB_SUCCESS;
  const ObiRole &role = root_server_.get_obi_role();
  if (role.get_role() == ObiRole::MASTER)
  {
    //6 init table __all_sys_stat
    if (OB_SUCCESS == ret)
    {
      if (OB_SUCCESS != (ret = init_all_sys_stat()))
      {
        TBSYS_LOG(ERROR, "failed to init all_sys_stat, err=%d", ret);
      }
      else
      {
        TBSYS_LOG(INFO, "init __all_sys_stat");
      }
    }
    // 7. init table __all_sys_param
    if (OB_SUCCESS == ret)
    {
      if (OB_SUCCESS != (ret = init_all_sys_param()))
      {
        TBSYS_LOG(ERROR, "failed to init all_sys_param, err=%d", ret);
      }
      else
      {
        TBSYS_LOG(INFO, "init __all_sys_param");
      }
    }
    // 8. init table __users
    if (OB_SUCCESS == ret)
    {
      if (OB_SUCCESS != (ret = init_users()))
      {
        TBSYS_LOG(ERROR, "failed to init users, err=%d", ret);
      }
      else
      {
        TBSYS_LOG(INFO, "init __users");
      }
    }
  }
  // 9. init table __all_cluster
  if (OB_SUCCESS == ret)
  {
    if (OB_SUCCESS != (ret = init_all_cluster()))
    {
      TBSYS_LOG(ERROR, "failed to init __all_cluster, err=%d", ret);
    }
    else
    {
      TBSYS_LOG(INFO, "init __all_cluster");
    }
  }
  // 10. init table __all_sys_config_stat
  if (OB_SUCCESS == ret)
  {
    if (OB_SUCCESS != (ret = init_all_sys_config_stat()))
    {
      TBSYS_LOG(ERROR, "failed to init __all_sys_config_stat, err=%d", ret);
    }
    else
    {
      TBSYS_LOG(INFO, "init __all_sys_config_stat");
    }
  }
  return ret;
}

int ObBootstrap::init_all_cluster()
{
  static const int64_t NETWORK_TIMEOUT = 1000000; /* 1s */
  const ObiRole &role = root_server_.get_obi_role();
  int ret = OB_SUCCESS;
  char buf[OB_MAX_SQL_LENGTH] = {0};
  ObString sql(sizeof (buf), 0, buf);
  const ObRootServerConfig &config = root_server_.get_config();
  ObServer rs;
  int64_t INIT_CLUSTER_FLOW_PERCENT = 0;
  if (role.get_role() == ObiRole::MASTER)
  {
    INIT_CLUSTER_FLOW_PERCENT = 100;
  }
  else
  {
    INIT_CLUSTER_FLOW_PERCENT = 0;
  }

  config.get_root_server(rs);
  if (OB_SUCCESS != (
        ret = ObInnerTableOperator::update_all_cluster(sql, config.cluster_id, rs, role,
                                                       INIT_CLUSTER_FLOW_PERCENT)))
  {
    TBSYS_LOG(ERROR, "Get update all_cluster sql failed! ret: [%d]", ret);
  }
  else
  {
    ObServer server;
    ObRootMsProvider ms_provider(const_cast<ObChunkServerManager&>(root_server_.get_server_manager()));
    ms_provider.init(const_cast<ObRootServerConfig&>(config),
        const_cast<ObRootRpcStub&>(root_server_.get_rpc_stub()));
    for (int64_t i = 0; i < config.retry_times; i++)
    {
      if (OB_SUCCESS != (ret = ms_provider.get_ms(server)))
      {
        TBSYS_LOG(WARN, "get merge server to init all_cluster table failed:ret[%d]", ret);
      }
      else if (OB_SUCCESS != root_server_.get_rpc_stub().execute_sql(server, sql, NETWORK_TIMEOUT))
      {
        TBSYS_LOG(WARN, "Try execute sql to init all_cluster table failed:server[%s], retry[%ld], ret[%d]",
            to_cstring(server), i, ret);
      }
      else
      {
        break;
      }
    }
  }

  if (OB_SUCCESS == ret)
  {
    char server_version[OB_SERVER_VERSION_LENGTH] = "";
    get_package_and_svn(server_version, sizeof(server_version));
    root_server_.commit_task(SERVER_ONLINE, OB_ROOTSERVER, rs, 0, server_version);
  }
  return ret;
}

int ObBootstrap::init_all_sys_config_stat()
{
  int ret = OB_SUCCESS;
  ObConfigManager *config_mgr = root_server_.get_config_mgr();
  int64_t version = tbsys::CTimeUtil::getTime();
  if (NULL == config_mgr)
  {
    ret = OB_NOT_INIT;
  }
  else
  {
    root_server_.log_worker_->got_config_version(version);
    ret = config_mgr->got_version(version);
  }
  return ret;
}

int ObBootstrap::init_users()
{
  int ret = OB_SUCCESS;
  nb_accessor::ObNbAccessor acc;
  if (OB_SUCCESS != (ret = acc.init(root_server_.schema_service_scan_helper_)))
  {
    TBSYS_LOG(WARN, "failed to init nb_accessor, err=%d", ret);
  }
  else
  {
    ObObj rowkey_objs[1];
    ObString username = ObString::make_string(OB_ADMIN_USER_NAME);
    rowkey_objs[0].set_varchar(username);
    ObRowkey rowkey;
    rowkey.assign(rowkey_objs, 1);

    char scrambled3[SCRAMBLE_LENGTH * 2 + 1];
    memset(scrambled3, 0, sizeof(scrambled3));
    ObString stored_pwd;
    stored_pwd.assign_ptr(scrambled3, SCRAMBLE_LENGTH * 2 + 1);
    ObString admin = ObString::make_string(OB_ADMIN_USER_NAME);
    ObEncryptedHelper::encrypt(stored_pwd, admin);
    stored_pwd.assign_ptr(scrambled3, SCRAMBLE_LENGTH * 2);
    // @bug
    if (OB_SUCCESS != (ret = acc.insert(OB_ALL_USER_TABLE_NAME, rowkey,
       KV("user_id", OB_ADMIN_UID) ("pass_word", stored_pwd) ("info", ObString::make_string("system administrator"))
         ("priv_all", 1) ("priv_alter", 1) ("priv_create", 1) ("priv_create_user", 1) ("priv_delete", 1)
         ("priv_drop", 1) ("priv_grant_option", 1) ("priv_insert", 1) ("priv_update", 1) ("priv_select", 1)
         ("priv_replace", 1) ("is_locked", 0))))
    {
      TBSYS_LOG(WARN, "failed to insert row into __users, err=%d", ret);
    }
  }
  return ret;
}

int ObBootstrap::init_all_sys_stat()
{
  int ret = OB_SUCCESS;
  nb_accessor::ObNbAccessor acc;
  if (OB_SUCCESS != (ret = acc.init(root_server_.schema_service_scan_helper_)))
  {
    TBSYS_LOG(WARN, "failed to init nb_accessor, err=%d", ret);
  }
  else
  {
    char string_value[STRING_VALUE_LENGTH] = "";
    ObObj rowkey_objs[2];
    rowkey_objs[0].set_int(0);// cluster_id
    rowkey_objs[1].set_varchar(ObString::make_string("ob_max_used_table_id")); // name
    ObRowkey rowkey;
    rowkey.assign(rowkey_objs, 2);
    snprintf(string_value, sizeof(string_value), "%lu", OB_APP_MIN_TABLE_ID + 2000);
    if (OB_SUCCESS != (ret = acc.insert(OB_ALL_SYS_STAT_TABLE_NAME, rowkey,
                                        KV("data_type", ObIntType)
                                        ("value", ObString::make_string(string_value))
                                        ("info", ObString::make_string("max used table id")))))
    {
      TBSYS_LOG(WARN, "failed to insert row into __all_sys_stat, err=%d", ret);
    }
    else
    {
      TBSYS_LOG(INFO, "insert max_used_table_id row into __all_sys_stat succ");
    }

    if (OB_SUCCESS == ret)
    {
      snprintf(string_value, sizeof(string_value), "%lu", OB_ADMIN_UID);
      rowkey_objs[1].set_varchar(ObString::make_string("ob_max_user_id")); // name
      if (OB_SUCCESS != (ret = acc.insert(OB_ALL_SYS_STAT_TABLE_NAME, rowkey,
                                          KV("data_type", ObIntType)
                                          ("value", ObString::make_string(string_value))
                                          ("info", ObString::make_string("max used user id")))))
      {
        TBSYS_LOG(WARN, "failed to insert row into __all_sys_stat, err=%d", ret);
      }
      else
      {
        TBSYS_LOG(INFO, "insert max_user_id row into __all_sys_stat succ");
      }
    }
    if (OB_SUCCESS == ret)
    {
      int64_t privilege_version = tbsys::CTimeUtil::getTime();
      snprintf(string_value, sizeof(string_value), "%ld", privilege_version);
      rowkey_objs[1].set_varchar(ObString::make_string("ob_current_privilege_version")); // name
      if (OB_SUCCESS != (ret = acc.insert(OB_ALL_SYS_STAT_TABLE_NAME, rowkey,
                                          KV("data_type", ObIntType)
                                          ("value", ObString::make_string(string_value))
                                          ("info", ObString::make_string("system's newest privilege version")))))
      {
        TBSYS_LOG(WARN, "failed to insert row into __all_sys_stat, err=%d", ret);
      }
      else
      {
        root_server_.set_privilege_version(privilege_version);
        TBSYS_LOG(INFO, "insert privilege_version row into __all_sys_stat succ");
      }
    }
  }
  return ret;
}

extern const char* svn_version();
extern const char* build_date();
extern const char* build_time();

int ObBootstrap::init_all_sys_param()
{
  int ret = OB_SUCCESS;
  nb_accessor::ObNbAccessor acc;
  if (OB_SUCCESS != (ret = acc.init(root_server_.schema_service_scan_helper_)))
  {
    TBSYS_LOG(WARN, "failed to init nb_accessor, err=%d", ret);
  }
  else
  {
#define INSERT_ALL_SYS_PARAM_ROW(ret, acc, cname, itype_value, cvalue, cinfo)  \
  if (ret == OB_SUCCESS) \
  { \
    ObObj rowkey_objs[2];       \
    rowkey_objs[0].set_int(0);  /* cluster_id */ \
    rowkey_objs[1].set_varchar(ObString::make_string(cname));  /* name */ \
    ObRowkey rowkey;  \
    rowkey.assign(rowkey_objs, 2); \
    if ((ret = acc.insert(OB_ALL_SYS_PARAM_TABLE_NAME, rowkey, \
                           KV("data_type", itype_value) \
                           ("value", ObString::make_string(cvalue)) \
                           ("info", ObString::make_string(cinfo)))) != OB_SUCCESS) \
    { \
      TBSYS_LOG(WARN, "failed to insert row into __all_sys_param, err=%d", ret); \
    } \
  }

    INSERT_ALL_SYS_PARAM_ROW(
        ret,
        acc,
        "ob_app_name",
        ObIntType,
        /* root_server_.config_.client_config_.app_name_, */
        "",
        "app name");
    INSERT_ALL_SYS_PARAM_ROW(
        ret,
        acc,
        "autocommit",
        ObIntType,
        "1",
        "");
    INSERT_ALL_SYS_PARAM_ROW(
        ret,
        acc,
        "character_set_results",
        ObVarcharType,
        "latin1",
        "");
    INSERT_ALL_SYS_PARAM_ROW(
        ret,
        acc,
        "sql_mode",
        ObVarcharType,
        "",
        "");
    INSERT_ALL_SYS_PARAM_ROW(
        ret,
        acc,
        "max_allowed_packet",
        ObIntType,
        "1048576",
        "");
    INSERT_ALL_SYS_PARAM_ROW(
        ret,
        acc,
        "ob_group_agg_push_down_param",
        ObBoolType,
        "true",
        "");
    INSERT_ALL_SYS_PARAM_ROW(
        ret,
        acc,
        "tx_isolation",
        ObVarcharType,
        "READ-COMMITTED",
        "Transaction Isolcation Levels: READ-UNCOMMITTED READ-COMMITTED REPEATABLE-READ SERIALIZABLE");
    INSERT_ALL_SYS_PARAM_ROW(
        ret,
        acc,
        "ob_tx_timeout",
        ObIntType,
        "100000000",
        "The max duration of one transaction");
    INSERT_ALL_SYS_PARAM_ROW(
        ret,
        acc,
        "ob_tx_idle_timeout",
        ObIntType,
        "100000000",
        "The max idle time between queries in one transaction");
    INSERT_ALL_SYS_PARAM_ROW(
        ret,
        acc,
        OB_READ_CONSISTENCY,
        ObIntType,
        "3",
        "read consistency level:4=STRONG, 3=WEAK, 2=FROZEN, 1=STATIC, 0=NONE");
    char version_comment[256];
    snprintf(version_comment, 256, "OceanBase %s (r%s) (Built %s %s)",
             PACKAGE_VERSION, svn_version(), build_date(), build_time());
    INSERT_ALL_SYS_PARAM_ROW(
        ret,
        acc,
        "version_comment",
        ObVarcharType,
        version_comment,
        "");
    INSERT_ALL_SYS_PARAM_ROW(
        ret,
        acc,
        "auto_increment_increment",
        ObIntType,
        "1",
        "");
    INSERT_ALL_SYS_PARAM_ROW(
        ret,
        acc,
        "ob_disable_create_sys_table",
        ObBoolType,
        "true",
        "");
    char query_timeout_str[64];
    snprintf(query_timeout_str, 64, "%ld", OB_DEFAULT_STMT_TIMEOUT);
    INSERT_ALL_SYS_PARAM_ROW(
        ret,
        acc,
        OB_QUERY_TIMEOUT_PARAM,
        ObIntType,
        query_timeout_str,
        "Query timeout in microsecond(us)");
    INSERT_ALL_SYS_PARAM_ROW(
        ret,
        acc,
        "wait_timeout",
        ObIntType,
        "0",
        "The number of seconds the server waits for activity on a noninteractive connection before closing it.");
    INSERT_ALL_SYS_PARAM_ROW(
        ret,
        acc,
        "interactive_timeout",
        ObIntType,
        "0",
        "The number of seconds the server waits for activity on an interactive connection before closing it.");
    INSERT_ALL_SYS_PARAM_ROW(
        ret,
        acc,
        "ob_charset",
        ObVarcharType,
        "gbk",
        "The Character set return to client");
#undef INSERT_ALL_SYS_PARAM_ROW
  }
  return ret;
}

int ObBootstrap::init_meta_file(const ObArray<ObServer> &created_cs)
{
  int ret = OB_SUCCESS;
  TBSYS_LOG(INFO, "init meta file");
  ObTabletMetaTableRow first_meta_row;
  first_meta_row.set_tid(OB_FIRST_TABLET_ENTRY_TID);
  first_meta_row.set_table_name(ObString::make_string(FIRST_TABLET_TABLE_NAME));
  first_meta_row.set_start_key(ObRowkey::MIN_ROWKEY);
  first_meta_row.set_end_key(ObRowkey::MAX_ROWKEY);
  for (int32_t i = 0; i < created_cs.count(); i ++)
  {
    ObTabletReplica r;
    r.version_ = 1;
    r.cs_ = created_cs.at(i);
    r.row_count_ = 0;
    r.occupy_size_ = 0;
    r.checksum_ = 0;
    if (OB_SUCCESS != (ret = first_meta_row.add_replica(r)))
    {
      TBSYS_LOG(ERROR, "failed to add replica for the meta row, err=%d", ret);
      break;
    }
    else
    {
      TBSYS_LOG(INFO, "add %dth replica to meta row", i);
    }
  }
  if (OB_SUCCESS == ret)
  {
    if (OB_SUCCESS != (ret = root_server_.get_first_meta()->init(first_meta_row)))
    {
      TBSYS_LOG(WARN, "failed to init first meta, err=%d", ret);
    }
    else if (OB_SUCCESS != (ret = log_worker_->init_first_meta(first_meta_row)))
    {
      TBSYS_LOG(WARN, "fail to sync log to slave err=%d.", ret);
    }
    else
    {
      TBSYS_LOG(INFO, "init first meta file success.");
    }
  }
  return ret;
}
