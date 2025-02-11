/*
 * Copyright (c) 2016 Cisco and/or its affiliates.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stddef.h>

#include <vnet/vnet.h>
#include <vnet/plugin/plugin.h>
#include <abf/abf_policy.h>
#include <abf/abf_itf_attach.h>
#include <vnet/mpls/mpls_types.h>
#include <vnet/fib/fib_path_list.h>
#include <vnet/fib/fib_api.h>

#include <vpp/app/version.h>

#include <vlibapi/api.h>
#include <vlibmemory/api.h>

/* define message IDs */
#include <vnet/format_fns.h>
#include <abf/abf.api_enum.h>
#include <abf/abf.api_types.h>

/**
 * Base message ID for the plugin
 */
static u32 abf_base_msg_id;

#define REPLY_MSG_ID_BASE (abf_base_msg_id)
#include <vlibapi/api_helper_macros.h>

static void
vl_api_abf_plugin_get_version_t_handler (vl_api_abf_plugin_get_version_t * mp)
{
  vl_api_abf_plugin_get_version_reply_t *rmp;
  vl_api_registration_t *rp;

  rp = vl_api_client_index_to_registration (mp->client_index);
  if (rp == 0)
    return;

  rmp = vl_msg_api_alloc (sizeof (*rmp));
  rmp->_vl_msg_id =
    ntohs (VL_API_ABF_PLUGIN_GET_VERSION_REPLY + abf_base_msg_id);
  rmp->context = mp->context;
  rmp->major = htonl (ABF_PLUGIN_VERSION_MAJOR);
  rmp->minor = htonl (ABF_PLUGIN_VERSION_MINOR);

  vl_api_send_msg (rp, (u8 *) rmp);
}

static void
vl_api_abf_policy_add_del_t_handler (vl_api_abf_policy_add_del_t * mp)
{
  vl_api_abf_policy_add_del_reply_t *rmp;
  fib_route_path_t *paths = NULL, *path;
  int rv = 0;
  u8 pi;

  if (mp->policy.n_paths == 0)
    {
      rv = VNET_API_ERROR_INVALID_VALUE;
      goto done;
    }

  vec_validate (paths, mp->policy.n_paths - 1);

  for (pi = 0; pi < mp->policy.n_paths; pi++)
    {
      path = &paths[pi];
      rv = fib_api_path_decode (&mp->policy.paths[pi], path);

      if (0 != rv)
	{
	  goto done;
	}
    }

  if (mp->is_add)
    {
      rv = abf_policy_update (ntohl (mp->policy.policy_id),
			      ntohl (mp->policy.acl_index), paths);
    }
  else
    {
      rv = abf_policy_delete (ntohl (mp->policy.policy_id), paths);
    }
done:
  vec_free (paths);

  REPLY_MACRO (VL_API_ABF_POLICY_ADD_DEL_REPLY);
}

static void
vl_api_abf_itf_attach_add_del_t_handler (vl_api_abf_itf_attach_add_del_t * mp)
{
  vl_api_abf_itf_attach_add_del_reply_t *rmp;
  fib_protocol_t fproto = (mp->attach.is_ipv6 ?
			   FIB_PROTOCOL_IP6 : FIB_PROTOCOL_IP4);
  int rv = 0;

  if (mp->is_add)
    {
      abf_itf_attach (fproto,
		      ntohl (mp->attach.policy_id),
		      ntohl (mp->attach.priority),
		      ntohl (mp->attach.sw_if_index));
    }
  else
    {
      abf_itf_detach (fproto,
		      ntohl (mp->attach.policy_id),
		      ntohl (mp->attach.sw_if_index));
    }

  REPLY_MACRO (VL_API_ABF_ITF_ATTACH_ADD_DEL_REPLY);
}

typedef struct abf_dump_walk_ctx_t_
{
  vl_api_registration_t *rp;
  u32 context;
} abf_dump_walk_ctx_t;

static int
abf_policy_send_details (u32 api, void *args)
{
  fib_path_encode_ctx_t walk_ctx = {
    .rpaths = NULL,
  };
  vl_api_abf_policy_details_t *mp;
  abf_dump_walk_ctx_t *ctx;
  fib_route_path_t *rpath;
  vl_api_fib_path_t *fp;
  size_t msg_size;
  abf_policy_t *ap;
  u8 n_paths;

  ctx = args;
  ap = abf_policy_get (api);
  n_paths = fib_path_list_get_n_paths (ap->ap_pl);
  msg_size = sizeof (*mp) + sizeof (mp->policy.paths[0]) * n_paths;

  mp = vl_msg_api_alloc (msg_size);
  clib_memset (mp, 0, msg_size);
  mp->_vl_msg_id = ntohs (VL_API_ABF_POLICY_DETAILS + abf_base_msg_id);

  /* fill in the message */
  mp->context = ctx->context;
  mp->policy.n_paths = n_paths;
  mp->policy.acl_index = htonl (ap->ap_acl);
  mp->policy.policy_id = htonl (ap->ap_id);

  fib_path_list_walk_w_ext (ap->ap_pl, NULL, fib_path_encode, &walk_ctx);

  fp = mp->policy.paths;
  vec_foreach (rpath, walk_ctx.rpaths)
  {
    fib_api_path_encode (rpath, fp);
    fp++;
  }

  vl_api_send_msg (ctx->rp, (u8 *) mp);

  vec_free (walk_ctx.rpaths);

  return (1);
}

static void
vl_api_abf_policy_dump_t_handler (vl_api_abf_policy_dump_t * mp)
{
  vl_api_registration_t *rp;

  rp = vl_api_client_index_to_registration (mp->client_index);
  if (rp == 0)
    return;

  abf_dump_walk_ctx_t ctx = {
    .rp = rp,
    .context = mp->context,
  };

  abf_policy_walk (abf_policy_send_details, &ctx);
}

static int
abf_itf_attach_send_details (u32 aiai, void *args)
{
  vl_api_abf_itf_attach_details_t *mp;
  abf_dump_walk_ctx_t *ctx;
  abf_itf_attach_t *aia;
  abf_policy_t *ap;

  ctx = args;
  aia = abf_itf_attach_get (aiai);
  ap = abf_policy_get (aia->aia_abf);

  mp = vl_msg_api_alloc (sizeof (*mp));
  mp->_vl_msg_id = ntohs (VL_API_ABF_ITF_ATTACH_DETAILS + abf_base_msg_id);

  mp->context = ctx->context;
  mp->attach.policy_id = htonl (ap->ap_id);
  mp->attach.sw_if_index = htonl (aia->aia_sw_if_index);
  mp->attach.priority = htonl (aia->aia_prio);
  mp->attach.is_ipv6 = (aia->aia_proto == FIB_PROTOCOL_IP6);

  vl_api_send_msg (ctx->rp, (u8 *) mp);

  return (1);
}

static void
vl_api_abf_itf_attach_dump_t_handler (vl_api_abf_itf_attach_dump_t * mp)
{
  vl_api_registration_t *rp;

  rp = vl_api_client_index_to_registration (mp->client_index);
  if (rp == 0)
    return;

  abf_dump_walk_ctx_t ctx = {
    .rp = rp,
    .context = mp->context,
  };

  abf_itf_attach_walk (abf_itf_attach_send_details, &ctx);
}

#include <abf/abf.api.c>

static clib_error_t *
abf_api_init (vlib_main_t * vm)
{
  /* Ask for a correctly-sized block of API message decode slots */
  abf_base_msg_id = setup_message_id_table ();

  return 0;
}

VLIB_INIT_FUNCTION (abf_api_init);

/* *INDENT-OFF* */
VLIB_PLUGIN_REGISTER () = {
    .version = VPP_BUILD_VER,
    .description = "Access Control List (ACL) Based Forwarding",
};
/* *INDENT-ON* */

/*
 * fd.io coding-style-patch-verification: ON
 *
 * Local Variables:
 * eval: (c-set-style "gnu")
 * End:
 */
