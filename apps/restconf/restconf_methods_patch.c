/*
 *
  ***** BEGIN LICENSE BLOCK *****
 
  Copyright (C) 2009-2019 Olof Hagsand
  Copyright (C) 2020-2021 Olof Hagsand and Rubicon Communications, LLC(Netgate), Siklu Ltd.

  This file is part of CLIXON.

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.

  Alternatively, the contents of this file may be used under the terms of
  the GNU General Public License Version 3 or later (the "GPL"),
  in which case the provisions of the GPL are applicable instead
  of those above. If you wish to allow use of your version of this file only
  under the terms of the GPL, and not to allow others to
  use your version of this file under the terms of Apache License version 2, 
  indicate your decision by deleting the provisions above and replace them with
  the  notice and other provisions required by the GPL. If you do not delete
  the provisions above, a recipient may use your version of this file under
  the terms of any one of the Apache License version 2 or the GPL.

  ***** END LICENSE BLOCK *****
  
 * Restconf YANG PATCH implementation  
 */


#ifdef HAVE_CONFIG_H
#include "clixon_config.h" /* generated by config & autoconf */
#endif
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <syslog.h>
#include <fcntl.h>
#include <time.h>
#include <signal.h>
#include <limits.h>
#include <sys/time.h>
#include <sys/wait.h>

/* cligen */
#include <cligen/cligen.h>

/* clicon */
#include <clixon/clixon.h>

#include "restconf_lib.h"
#include "restconf_handle.h"
#include "restconf_api.h"
#include "restconf_err.h"
#include "restconf_methods.h"
#include "restconf_methods_post.h"
#include "restconf_methods_patch.h"

#ifdef YANG_PATCH

/*! Return a value within XML tags
 * @param [in] nsc  namespace context
 * @param [in] xn   cxobj containing XML with the current edit
 * @param [in] val   cbuf to which the value will be written
 * @param [in] key  string containing the tag
 * @retval 0 success
 * @retval <0 failure
 */
static int
yang_patch_get_xval(cvec       *nsc,
		    cxobj      *xn,
		    cbuf       *val,
		    const char *key)
{
    cxobj **vec = NULL;
    size_t  veclen = 0;
    char*   tmp_val = NULL;
    int     ret;
    cxobj  *xn_tmp = NULL;

    if ((ret = xpath_vec(xn, nsc, "%s", &vec, &veclen, key)) < 0)
        return ret;
    if (veclen == 1){  //veclen should always be 1
        xn_tmp = vec[0];
        tmp_val = xml_body(xn_tmp);
        cbuf_append_str(val, tmp_val);
    }
    return 0;
}

/*! Add square brackets after the surrounding curly brackets in JSON
  * Needed, in order to modify the result of xml2json_cbuf() to be valid input
  * to api_data_post() and api_data_write()
  * @param [in]  x_simple_patch  a cxobj to pass to xml2json_cbuf()
  * @retval new cbuf with the modified json
  * @retval NULL   Error
  */
static cbuf*
yang_patch_xml2json_modified_cbuf(cxobj *x_simple_patch)
{
    cbuf *json_simple_patch = NULL;
    cbuf *cb = NULL;
    char *json_simple_patch_tmp;
    int   brace_count = 0;
	
    json_simple_patch = cbuf_new();
    if (json_simple_patch == NULL)
        return NULL;
    cb = cbuf_new();
    xml2json_cbuf(cb, x_simple_patch, 1);

    // Insert a '[' after the first '{' to get the JSON to match what api_data_post/write() expect
    json_simple_patch_tmp = cbuf_get(cb);
    for (int l = 0; l < strlen(json_simple_patch_tmp); l++) {
        char c = json_simple_patch_tmp[l];
        if (c == '{') {
            brace_count++;
            if (brace_count == 2) { // We've reached the second brace, insert a '[' before it
                cbuf_append(json_simple_patch,(int)'[');
            }
        }
        cbuf_append(json_simple_patch,(int)c);
    }

    // Insert a ']' before the last '}' to get the JSON to match what api_data_post() expects
    for (int l = cbuf_len(json_simple_patch) - 1; l >= 0; l--) {
        char c = cbuf_get(json_simple_patch)[l];
        if (c == '}') {
            // Truncate and add a string, as there is not a function to insert a char into a cbuf
	    cbuf_trunc(json_simple_patch, l);
	    cbuf_append_str(json_simple_patch, "]}");
	    break;
        }
    }
    cbuf_free(cb);
    return json_simple_patch;
}

/*!yang_patch_strip_after_last_slash 
 *
 * Strip /... from end  of val
 * so that e.g. "/interface=eth2" becomes "/"
 * or "/interface_list=mylist/interface=eth2" becomes "/interface_list=mylist/"
 *
 * @param[in]  val         value to strip
 * @retval new cbuf with the stripped string
 * @retval NULL error
 */
static cbuf*
yang_patch_strip_after_last_slash(cbuf* val)
{
    cbuf *cb;
    cbuf *val_tmp;
    int   idx;
    
    cb = cbuf_new();
    val_tmp = cbuf_new();
    cbuf_append_str(val_tmp, cbuf_get(val));
    idx = cbuf_len(val_tmp);
    for (int l = cbuf_len(val_tmp) - 1; l>= 0; l--) {
	if (cbuf_get(val_tmp)[l] == '/') {
	    idx = l;
	    break;
	}
    }
    if (idx == cbuf_len(val_tmp)) // Didn't find a slash in the loop above
	return NULL;
    cbuf_trunc(val_tmp, idx + 1);
    if (cbuf_append_str(cb, cbuf_get(val_tmp)) < 0)
	return NULL;
    cbuf_free(val_tmp);
    return cb;
}

/*! YANG PATCH replace method
 * @param[in]  h         Clixon handle
 * @param[in]  req       Generic Www handle
 * @param[in]  pi        Offset, where to start pcvec
 * @param[in]  qvec      Vector of query string (QUERY_STRING)
 * @param[in]  pretty    Set to 1 for pretty-printed xml/json output
 * @param[in]  media_out Output media
 * @param[in]  ds       0 if "data" resource, 1 if rfc8527 "ds" resource
 * @param[in]  simplepatch_request_uri URI for patch request, e.g. "/restconf/data/ietf-interfaces:interfaces"
 * @param[in]  target_val       value in "target" field of edit in YANG patch
 * @param[in]  value_vec_len    number of elements in the "value" array of an edit in YANG patch
 * @param[in]  value_vec        pointer to the "value" array of an edit in YANG patch
 * @param[in]  x_simple_patch   pointer to XML containing module name, e.g. <ietf-interfaces:interface/>
 */
static int
yang_patch_do_replace (clicon_handle  h,
		       void          *req,
		       int            pi,
		       cvec          *qvec,
		       int            pretty,
		       restconf_media media_out,
		       ietf_ds_t      ds,
		       cbuf          *simple_patch_request_uri,
		       cbuf          *target_val,       
		       int            value_vec_len,
		       cxobj        **value_vec,
		       cxobj         *x_simple_patch
		       )
{
    cxobj *value_vec_tmp = NULL;
    cbuf  *delete_req_uri = NULL;
    int    ret;
    cbuf  *post_req_uri = NULL;
    cbuf  *json_simple_patch = NULL;
    
    delete_req_uri = cbuf_new();
    if (delete_req_uri == NULL)
	return 1;

    // Make delete_req_uri something like "/restconf/data/ietf-interfaces:interfaces"
    if (cbuf_append_str(delete_req_uri, cbuf_get(simple_patch_request_uri)) < 0)
	return 1;

    // Add the target to delete_req_uri,
    // so it's something like "/restconf/data/ietf-interfaces:interfaces/interface=eth2"
    if (cbuf_append_str(delete_req_uri, cbuf_get(target_val)) <  0)
	return 1;

    // Delete the object with the old values
    ret = api_data_delete(h, req, cbuf_get(delete_req_uri), pi, pretty, YANG_DATA_JSON, ds );
    cbuf_free(delete_req_uri);
    if (ret != 0)
	return ret;

    // Now set up for the post request.
    // Strip /... from end  of target val
    // so that e.g. "/interface=eth2" becomes "/"
    // or "/interface_list=mylist/interface=eth2" becomes "/interface_list=mylist/"
    post_req_uri = yang_patch_strip_after_last_slash(target_val);

    // Make post_req_uri something like "/restconf/data/ietf-interfaces:interfaces"
    if (cbuf_append_str(simple_patch_request_uri, cbuf_get(post_req_uri)))
	return 1;
    cbuf_free(post_req_uri);

    // Now insert the new values into the data
    // (which will include the key value and all other mandatory values)
    for (int k = 0; k < value_vec_len; k++) {
	if (value_vec[k] != NULL) {
	    value_vec_tmp = xml_dup(value_vec[k]);
	    xml_addsub(x_simple_patch, value_vec_tmp);
	}
    }
    // Convert the data to json
    json_simple_patch = cbuf_new();
    if (json_simple_patch == NULL)
	return 1;
    xml2json_cbuf(json_simple_patch, x_simple_patch, 1);

    // Send the POST request
    ret = api_data_post(h, req, cbuf_get(simple_patch_request_uri), pi, qvec, cbuf_get(json_simple_patch), pretty, YANG_DATA_JSON, media_out, ds );
      
    cbuf_free(json_simple_patch);
    xml_free(value_vec_tmp);
    return ret;
}

/*! YANG PATCH create method
 * @param[in]  h         Clixon handle
 * @param[in]  req       Generic Www handle
 * @param[in]  pi        Offset, where to start pcvec
 * @param[in]  qvec      Vector of query string (QUERY_STRING)
 * @param[in]  pretty    Set to 1 for pretty-printed xml/json output
 * @param[in]  media_out Output media
 * @param[in]  ds       0 if "data" resource, 1 if rfc8527 "ds" resource
 * @param[in]  simplepatch_request_uri URI for patch request, e.g. "/restconf/data/ietf-interfaces:interfaces"
 * @param[in]  value_vec_len    number of elements in the "value" array of an edit in YANG patch
 * @param[in]  value_vec        pointer to the "value" array of an edit in YANG patch
 * @param[in]  x_simple_patch   pointer to XML containing module name, e.g. <ietf-interfaces:interface/>
 */
static int
yang_patch_do_create (clicon_handle  h,
		      void          *req,
		      int            pi,
		      cvec          *qvec,
		      int            pretty,
		      restconf_media media_out,
		      ietf_ds_t      ds,
		      cbuf          *simple_patch_request_uri,
		      int            value_vec_len,
		      cxobj        **value_vec,
		      cxobj         *x_simple_patch
		      )
{
    int    retval = -1;
    cxobj *value_vec_tmp = NULL;
    cbuf  *cb = NULL;
    char  *json_simple_patch;
    
    for (int k = 0; k < value_vec_len; k++) {
	if (value_vec[k] != NULL) {
	    value_vec_tmp = xml_dup(value_vec[k]);
	    xml_addsub(x_simple_patch, value_vec_tmp);
	}
    }
    // Send the POST request
    if ((cb = cbuf_new()) == NULL){
	clicon_err(OE_UNIX, errno, "cbuf_new");
	goto done;
    }
    if (xml2json_cbuf(cb, x_simple_patch, 1) < 0)
	goto done;
    json_simple_patch = cbuf_get(cb);
    if (api_data_post(h, req, cbuf_get(simple_patch_request_uri),
		      pi, qvec,
		      json_simple_patch, pretty, YANG_DATA_JSON, media_out, ds) < 0)
	goto done;
    xml_free(value_vec_tmp);
     retval = 0;
 done:
    return retval;
}

/*! YANG PATCH insert method
 * @param[in]  h         Clixon handle
 * @param[in]  req       Generic Www handle
 * @param[in]  pi        Offset, where to start pcvec
 * @param[in]  pretty    Set to 1 for pretty-printed xml/json output
 * @param[in]  media_out Output media
 * @param[in]  ds       0 if "data" resource, 1 if rfc8527 "ds" resource
 * @param[in]  simple_patch_request_uri URI for patch request, e.g. "/restconf/data/ietf-interfaces:interfaces"
 * @param[in]  value_vec_len    number of elements in the "value" array of an edit in YANG patch
 * @param[in]  value_vec        pointer to the "value" array of an edit in YANG patch
 * @param[in]  x_simple_patch   pointer to XML containing module name, e.g. <ietf-interfaces:interface/>
 * @param[in]  where_val       value in "where" field of edit in YANG patch
 * @param[in]  api_path        full API path, e.g. "/restconf/data/example-jukebox:jukebox/playlist=Foo-One" 
 * @param[in]  point_val       value in "point" field of edit in YANG patch
 */
static int
yang_patch_do_insert (clicon_handle  h,
		      void          *req,
		      int            pi,
		      int            pretty,
		      restconf_media media_out,
		      ietf_ds_t      ds,
		      cbuf          *simple_patch_request_uri,
		      int            value_vec_len,
		      cxobj        **value_vec,
		      cxobj         *x_simple_patch,
		      cbuf          *where_val,
		      char          *api_path,
		      cbuf          *point_val
      )
{
     cxobj  *value_vec_tmp = NULL;
     cbuf   *json_simple_patch;
     cg_var *cv;
     cbuf   *point_str = NULL;
     int     ret;
     
     // Loop through the XML, and get each value
     for (int k = 0; k < value_vec_len; k++) {
         if (value_vec[k] != NULL) {
             value_vec_tmp = xml_dup(value_vec[k]);
             xml_addsub(x_simple_patch, value_vec_tmp);
         }
     }
     json_simple_patch = yang_patch_xml2json_modified_cbuf(x_simple_patch);
     if (json_simple_patch == NULL)
         return 1;

     // Set the insert attributes
     cvec* qvec_tmp = NULL;
     qvec_tmp = cvec_new(0);
     if (qvec_tmp == NULL)
         return 1;

     if ((cv = cvec_add(qvec_tmp, CGV_STRING)) == NULL){
         return 1;
     }
     cv_name_set(cv, "insert");
     cv_string_set(cv, cbuf_get(where_val));
     point_str = cbuf_new();
     if (point_str == NULL)
         return 1;
     cbuf_append_str(point_str, api_path);
     cbuf_append_str(point_str, cbuf_get(point_val));
     if ((cv = cvec_add(qvec_tmp, CGV_STRING)) == NULL){
         return 1;
     }
     cv_name_set(cv, "point");
     cv_string_set(cv, cbuf_get(point_str));

     // Send the POST request
     ret = api_data_post(h, req, cbuf_get(simple_patch_request_uri), pi, qvec_tmp, cbuf_get(json_simple_patch), pretty, YANG_DATA_JSON, media_out, ds );
     xml_free(value_vec_tmp);
     cbuf_free(point_str);
     cbuf_free(json_simple_patch);
     return ret;
}

/*! YANG PATCH merge method
 * @param[in]  h         Clixon handle
 * @param[in]  req       Generic Www handle
 * @param[in]  pcvec    Vector of path ie DOCUMENT_URI element
 * @param[in]  pi        Offset, where to start pcvec
 * @param[in]  qvec      Vector of query string (QUERY_STRING)
 * @param[in]  pretty    Set to 1 for pretty-printed xml/json output
 * @param[in]  media_out Output media
 * @param[in]  ds       0 if "data" resource, 1 if rfc8527 "ds" resource
 * @param[in]  simple_patch_request_uri URI for patch request, e.g. "/restconf/data/ietf-interfaces:interfaces"
 * @param[in]  value_vec_len    number of elements in the "value" array of an edit in YANG patch
 * @param[in]  value_vec        pointer to the "value" array of an edit in YANG patch
 * @param[in]  x_simple_patch   pointer to XML containing module name, e.g. "<ietf-interfaces:interface/>"
 * @param[in]  where_val       value in "where" field of edit in YANG patch
 * @param[in]  key_xn          XML with key tag and value, e.g. "<name>Foo-One</name>"
 */
static int
yang_patch_do_merge(clicon_handle h,
		    void         *req,
		    cvec         *pcvec,
		    int           pi,
		    cvec         *qvec,
		    int           pretty,
		    restconf_media media_out,
		    ietf_ds_t     ds,
		    cbuf* simple_patch_request_uri,
		    int value_vec_len,
		    cxobj** value_vec,
		    cxobj *x_simple_patch,
		    cxobj *key_xn
		    )
{
    int    ret = -1;
    cxobj *value_vec_tmp = NULL;
    cbuf  *cb = NULL;
    cbuf  *json_simple_patch = NULL;
	
    if (key_xn != NULL)
	xml_addsub(x_simple_patch, key_xn);
    
    // Loop through the XML, create JSON from each one, and submit a simple patch
    for (int k = 0; k < value_vec_len; k++) {
	if (value_vec[k] != NULL) {
	    value_vec_tmp = xml_dup(value_vec[k]);
	    xml_addsub(x_simple_patch, value_vec_tmp);
	}
	cb = cbuf_new();
	xml2json_cbuf(cb, x_simple_patch, 1);

        json_simple_patch = yang_patch_xml2json_modified_cbuf(x_simple_patch);
        if (json_simple_patch == NULL)
            return 1;
	xml_free(value_vec_tmp);
	// Send the simple patch request
	ret = api_data_write(h, req, cbuf_get(simple_patch_request_uri), pcvec, pi, qvec, cbuf_get(json_simple_patch), pretty, YANG_DATA_JSON, media_out, 1, ds );
	cbuf_free(cb);
	cbuf_free(json_simple_patch);
    }
    return ret;
}

/*! YANG PATCH method
 * @param[in]  h         Clixon handle
 * @param[in]  req       Generic Www handle
 * @param[in]  api_path0 According to restconf (Sec 3.5.3.1 in rfc8040)
 * @param[in]  pcvec     Vector of path ie DOCUMENT_URI element
 * @param[in]  pi        Offset, where to start pcvec
 * @param[in]  qvec      Vector of query string (QUERY_STRING)
 * @param[in]  data      Stream input data
 * @param[in]  pretty    Set to 1 for pretty-printed xml/json output
 * @param[in]  media_out Output media
 * Netconf:  <edit-config> (nc:operation="merge")
 * See RFC8072
 * YANG patch can be used to "create", "delete", "insert", "merge", "move", "replace", and/or
   "remove" a resource within the target resource.
 * Currently "move" not supported
 */
int
api_data_yang_patch(clicon_handle h,
		    void         *req,
		    char         *api_path0,
		    cvec         *pcvec,
		    int           pi,
		    cvec         *qvec,
		    char         *data,
		    int           pretty,
		    restconf_media media_out,
		    ietf_ds_t     ds)
{
    int            retval = -1;
    int            i;
    cxobj         *xdata0 = NULL; /* Original -d data struct (including top symbol) */
    cbuf          *cbx = NULL;
    cxobj         *xtop = NULL; /* top of api-path */
    cxobj         *xbot = NULL; /* bottom of api-path */
    yang_stmt     *ybot = NULL; /* yang of xbot */
    cxobj         *xbot_tmp = NULL;
    yang_stmt     *yspec;
    char          *api_path;
    cxobj         *xret = NULL;
    cxobj         *xretcom = NULL; /* return from commit */
    cxobj         *xretdis = NULL; /* return from discard-changes */
    cxobj         *xerr = NULL;    /* malloced must be freed */
    int            ret;
    cvec          *nsc = NULL;
    yang_bind      yb;
    char          *xpath = NULL;
    cbuf          *path_orig_1 = NULL;
    char           yang_patch_path[] = "/ietf-yang-patch:yang-patch";
    int            nrchildren0 = 0;
    cxobj         *x = NULL;
    size_t         veclen;
    cxobj        **vec = NULL;

    clicon_debug(1, "%s api_path:\"%s\"",  __FUNCTION__, api_path0);
    if ((yspec = clicon_dbspec_yang(h)) == NULL){
	clicon_err(OE_FATAL, 0, "No DB_SPEC");
	goto done;
    }
    api_path=api_path0;
    /* strip /... from start */
    for (i=0; i<pi; i++)
	api_path = index(api_path+1, '/');
    /* Translate yang-patch path to xpath: xpath (cbpath) and namespace context (nsc) */

    if ((ret = api_path2xpath(yang_patch_path, yspec, &xpath, &nsc, &xerr)) < 0)
	goto done;
    if (ret == 0){ /* validation failed */
	if (api_return_err0(h, req, xerr, pretty, media_out, 0) < 0)
	    goto done;
	goto ok;
    }
    /* Create config top-of-tree */
    if ((xtop = xml_new(NETCONF_INPUT_CONFIG, NULL, CX_ELMNT)) == NULL)
	goto done;

    /* Translate yang-patch path to xml in the form of xtop/xbot */
    xbot = xtop;
    if ((ret = api_path2xml(yang_patch_path, yspec, xtop, YC_DATANODE, 1, &xbot, &ybot, &xerr)) < 0)
	goto done;
    if (ret == 0){ /* validation failed */
	if (api_return_err(h, req, xerr, pretty, media_out, 0) < 0)
	    goto done;
	goto ok;
    }
 
    yb = YB_MODULE;
    if ((ret = clixon_json_parse_string(data, yb, yspec, &xbot, &xerr)) < 0){
	if (netconf_malformed_message_xml(&xerr, clicon_err_reason) < 0)
	    goto done;
	if (api_return_err0(h, req, xerr, pretty, media_out, 0) < 0)
	    goto done;
	goto ok;
    }
    if (ret == 0){
	if (api_return_err0(h, req, xerr, pretty, media_out, 0) < 0)
	    goto done;
	goto ok;
    }
    /* 
     * RFC 8072 2.1: The message-body MUST identify exactly one resource instance
     */
    if (xml_child_nr_type(xbot, CX_ELMNT) - nrchildren0 != 1){
	if (netconf_malformed_message_xml(&xerr, "The message-body MUST contain exactly one instance of the expected data resource") < 0)
	    goto done;
	if (api_return_err0(h, req, xerr, pretty, media_out, 0) < 0)
	    goto done;
	goto ok;
    }

    while ((x = xml_child_each(xbot, x, CX_ELMNT)) != NULL){
	ret = xpath_vec(x, nsc, "edit", &vec, &veclen);
	if (xml_flag(x, XML_FLAG_MARK)){
	    xml_flag_reset(x, XML_FLAG_MARK);
	    continue;
	}
    }
    path_orig_1 = cbuf_new();
    if (path_orig_1 == NULL) {
        goto done;
    } else {
        cbuf_append_str(path_orig_1, restconf_uripath(h));
    }

    // Loop through the edits
    for (int i = 0; i < veclen; i++) {
        cxobj **tmp_vec = NULL;
        size_t tmp_veclen = 0;
        cxobj *xn = vec[i];
	clicon_log_xml(LOG_DEBUG, xn, "%s %d xn:", __FUNCTION__, __LINE__);
        // Get target
        cbuf *target_val = cbuf_new();
        ret = yang_patch_get_xval(nsc, xn, target_val, "target");
        if (ret < 0) {
            goto done;
        }
        // Get operation
        cbuf *op_val = cbuf_new();
        ret = yang_patch_get_xval(nsc, xn, op_val, "operation");
        if (ret < 0) {
            goto done;
        }
        // Get "point" and "where" for insert operations
        cbuf *point_val = NULL;
        cbuf *where_val = cbuf_new();
        if (strcmp(cbuf_get(op_val), "insert") == 0) {
            point_val = cbuf_new();
            ret = yang_patch_get_xval(nsc, xn, point_val, "point");
            if (ret < 0) {
                goto done;
            }
            where_val = cbuf_new();
            ret = yang_patch_get_xval(nsc, xn, where_val, "where");
            if (ret < 0) {
                goto done;
            }
        }

        // Construct request URI
        cbuf* simple_patch_request_uri = cbuf_new();
        cbuf_append_str(simple_patch_request_uri, cbuf_get(path_orig_1));

        cbuf* api_path_target = cbuf_new();
        cbuf_append_str(api_path_target, api_path);
        if (strcmp(cbuf_get(op_val), "merge") == 0) {
            cbuf_append_str(api_path_target, cbuf_get(target_val));
            cbuf_append_str(simple_patch_request_uri, cbuf_get(target_val));
        }

        if (xerr)
	    xml_free(xerr);
        if ((xtop = xml_new(NETCONF_INPUT_CONFIG, NULL, CX_ELMNT)) == NULL)
	    goto done;

        // Get key field
        /* Translate api_path to xml in the form of xtop/xbot */
        xbot_tmp = xtop;
	if ((ret = api_path2xml(cbuf_get(api_path_target), yspec, xtop, YC_DATANODE, 1, &xbot_tmp, &ybot, &xerr)) < 0)
	    goto done;
	if (ret == 0){ /* validation failed */
	    if (api_return_err0(h, req, xerr, pretty, media_out, 0) < 0)
	    	goto done;
	    goto ok;
	}
        char *key_node_id = xml_name(xbot_tmp);
        char *path = NULL;
        if ((path = restconf_param_get(h, "REQUEST_URI")) != NULL){
            for (int i1 = 0; i1 <pi; i1++)
	        path = index(path+1, '/');
        }
        const char colon[2] = ":";
        char *modname = strtok(&(path[1]), colon);

        cxobj **key_vec = NULL;

        key_vec = xml_childvec_get(xbot_tmp);
        cxobj *key_xn = NULL;
        if (key_vec != NULL) {
            key_xn = key_vec[0];
        }
        // Get values (for "delete" and "remove", there are no values)
        xpath_vec(xn, nsc, "value", &tmp_vec, &tmp_veclen);
        key_node_id = NULL;

        // Loop through the values
        for (int j = 0; j < tmp_veclen; j++) {
            cxobj *values_xn = tmp_vec[j];
            cxobj** values_child_vec = xml_childvec_get(values_xn);
            if (key_node_id == NULL)
                key_node_id = xml_name(*values_child_vec);

            cbuf *patch_header = cbuf_new();
            if (patch_header == NULL) {
                goto done;
            }
            cbuf_append_str(patch_header, modname);
            cbuf_append_str(patch_header, ":");
            cbuf_append_str(patch_header, key_node_id);
            cxobj *x_simple_patch = xml_new(cbuf_get(patch_header), NULL, CX_ELMNT);
            if (x_simple_patch == NULL)
                goto done;
            int value_vec_len = xml_child_nr(*values_child_vec);
            cxobj** value_vec = xml_childvec_get(*values_child_vec);
            // For "replace", delete the item and then POST it
            // TODO - in an ordered list, insert it into its original position
            if (strcmp(cbuf_get(op_val),"replace") == 0) {
                ret = yang_patch_do_replace(h, req, pi, qvec, pretty, media_out, ds, simple_patch_request_uri, target_val, value_vec_len, value_vec, x_simple_patch);
                if (ret != 0) {
                    goto done;
                }
            }
            // For "create", put all the data values into a single POST request
            if (strcmp(cbuf_get(op_val),"create") == 0) {
                ret = yang_patch_do_create(h, req, pi, qvec, pretty, media_out, ds, simple_patch_request_uri, value_vec_len, value_vec, x_simple_patch);
                if (ret != 0) {
                    goto done;
                }
            }
            // For "insert", make a api_data_post request
            if (strcmp(cbuf_get(op_val), "insert") == 0) {
                ret = yang_patch_do_insert(h, req, pi, pretty, media_out, ds, simple_patch_request_uri, value_vec_len, value_vec, x_simple_patch, where_val, api_path, point_val);
                if (ret != 0) {
                    goto done;
                }
            }
            // For merge", make single simple patch requests for each value
            if (strcmp(cbuf_get(op_val),"merge") == 0) {
                ret = yang_patch_do_merge(h, req, pcvec, pi, qvec, pretty, media_out, ds, simple_patch_request_uri, value_vec_len, value_vec, x_simple_patch, key_xn);
                if (ret != 0) {
                    goto done;
                }
            }
            cbuf_free(patch_header);
	    if (x_simple_patch)
		free(x_simple_patch);
        }
        if ((strcmp(cbuf_get(op_val), "delete") == 0) ||
            (strcmp(cbuf_get(op_val), "remove") == 0)) {
	    cbuf_append_str(simple_patch_request_uri, cbuf_get(target_val));
	    if (strcmp(cbuf_get(op_val), "delete") == 0) {
		// TODO - send error
	    } else {
		// TODO - do not send error
	    }
	    api_data_delete(h, req, cbuf_get(simple_patch_request_uri), pi, pretty, YANG_DATA_JSON, ds); 
        }
        cbuf_free(simple_patch_request_uri);
        cbuf_free(api_path_target);
        cbuf_free(target_val);
        cbuf_free(op_val);
        cbuf_free(point_val);
        cbuf_free(where_val);
    }
 ok:
    retval = 0;
 done:
    cbuf_free(path_orig_1);
    if (vec)
	free(vec);
    if (xpath)
	free(xpath);
    if (nsc)
	xml_nsctx_free(nsc);
    if (xret)
	xml_free(xret);
    if (xerr)
	xml_free(xerr);
    if (xretcom)
	xml_free(xretcom);
    if (xretdis)
	xml_free(xretdis);
    if (xtop)
	xml_free(xtop);
    if (xdata0)
	xml_free(xdata0);
    if (cbx)
	cbuf_free(cbx); 
    return retval;
}

#else // YANG_PATCH

int
api_data_yang_patch(clicon_handle h,
		    void         *req,
		    char         *api_path0,
		    cvec         *pcvec,
		    int           pi,
		    cvec         *qvec,
		    char         *data,
		    int           pretty,
		    restconf_media media_out,
		    ietf_ds_t     ds)
{
    clicon_err(OE_RESTCONF, 0, "Not implemented");
    return -1;
}
#endif // YANG_PATCH
