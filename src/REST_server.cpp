/***************************************************************************
 *
 * Project:  OpenCPN
 * Purpose:  Implement RESTful server.
 * Author:   David Register, Alec Leamas
 *
 ***************************************************************************
 *   Copyright (C) 2022 by David Register, Alec Leamas                     *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301,  USA.         *
 **************************************************************************/

#include <mutex>
#include <vector>
#include <memory>
#include <condition_variable>
#include <thread>

#include <wx/event.h>
#include <wx/log.h>
#include <wx/string.h>
#include <wx/thread.h>
#include <wx/utils.h>
#include <fstream>
#include <string>

#include "REST_server.h"
#include "mongoose.h"
#include "gui_lib.h"
#include "REST_server_gui.h"
#include "pugixml.hpp"
#include "route.h"
#include "routeman.h"
#include "nav_object_database.h"

Route *GPXLoadRoute1(pugi::xml_node &wpt_node, bool b_fullviz,
                            bool b_layer, bool b_layerviz, int layer_id,
                            bool b_change);

bool InsertRouteA(Route *pTentRoute, NavObjectCollection1* navobj);

extern Routeman *g_pRouteMan;

//  Some global variables to handle thread syncronization
int return_status;
std::condition_variable return_status_condition;
std::mutex mx;



class RESTServerThread : public wxThread {
public:
  RESTServerThread(RESTServer* Launcher);

  ~RESTServerThread(void);
  void* Entry();
  void OnExit(void);

private:
  RESTServer *m_pParent;

};


class RESTServerEvent;
wxDECLARE_EVENT(wxEVT_RESTFUL_SERVER, RESTServerEvent);

class RESTServerEvent : public wxEvent {
public:
  RESTServerEvent(
      wxEventType commandType = wxEVT_RESTFUL_SERVER, int id = 0)
      : wxEvent(id, commandType){};
  ~RESTServerEvent(){};

  // accessors
  void SetPayload(std::shared_ptr<std::string> data) {
    m_payload = data;
  }
  void SetSource(std::string source){
    m_source_peer = source;
  }
  std::shared_ptr<std::string> GetPayload() { return m_payload; }

  // required for sending with wxPostEvent()
  wxEvent* Clone() const {
    RESTServerEvent* newevent =
        new RESTServerEvent(*this);
    newevent->m_payload = this->m_payload;
    newevent->m_source_peer = this->m_source_peer;
    return newevent;
  };

  std::shared_ptr<std::string> m_payload;
  std::string m_source_peer;
private:


};

wxDEFINE_EVENT(wxEVT_RESTFUL_SERVER, RESTServerEvent);

//========================================================================
/*    RESTServer implementation
 * */

RESTServer::RESTServer()
    :  m_Thread_run_flag(-1)
{

  // Prepare the wxEventHandler to accept events from the actual hardware thread
  Bind(wxEVT_RESTFUL_SERVER, &RESTServer::HandleServerMessage,
       this);

}

RESTServer::~RESTServer() { }

bool RESTServer::StartServer(std::string certificate_location) {

  m_certificate_directory = certificate_location;
  m_cert_file = m_certificate_directory + std::string("cert.pem");       // Certificate PEM file
  m_key_file = m_certificate_directory + std::string("key.pem");     // The key PEM file


  //    Kick off the  Server thread
  SetSecondaryThread(new RESTServerThread(this));
  SetThreadRunFlag(1);
  GetSecondaryThread()->Run();

  return true;
}

void RESTServer::StopServer() {
  wxLogMessage(
      wxString::Format(_T("Stopping REST service")));

  Unbind(wxEVT_RESTFUL_SERVER, &RESTServer::HandleServerMessage,
       this);

  //    Kill off the Secondary RX Thread if alive
  if (m_pSecondary_Thread) {
    m_pSecondary_Thread->Delete();

    if (m_bsec_thread_active)  // Try to be sure thread object is still alive
    {
      wxLogMessage(_T("Stopping Secondary Thread"));

      m_Thread_run_flag = 0;

      int tsec = 10;
      while ((m_Thread_run_flag >= 0) && (tsec--)) wxSleep(1);

      wxString msg;
      if (m_Thread_run_flag < 0)
        msg.Printf(_T("Stopped in %d sec."), 10 - tsec);
      else
        msg.Printf(_T("Not Stopped after 10 sec."));
      wxLogMessage(msg);
    }

    m_pSecondary_Thread = NULL;
    m_bsec_thread_active = false;
  }
}

void RESTServer::HandleServerMessage(RESTServerEvent& event) {
  auto p = event.GetPayload();
  std::string *payload = p.get();

  //printf("%s\n", payload->c_str());

  // Server thread is waiting for (return_status >= 0) on notify_one()
  int return_stat = RESTServerResult::RESULT_GENERIC_ERROR;      // generic error

#ifndef CLIAPP
  // GUI dialogs can go here....
  AcceptObjectDialog dialog1(NULL, wxID_ANY, _("OpenCPN Server Message"),
    "", wxDefaultPosition, wxDefaultSize, SYMBOL_STG_STYLE );

  wxString hmsg(event.m_source_peer.c_str());
  hmsg += " has sent you a new route.\nAccept?";

  dialog1.SetMessage(hmsg);
  dialog1.SetCheck1Message(_("Always accept objects from this source?"));

  if (dialog1.ShowModal() == ID_STG_OK) {
    Route *pRoute = NULL;

      // Load the GPX file
    pugi::xml_document doc;
    pugi::xml_parse_result result = doc.load_buffer(payload->c_str(), payload->size());
    if (result.status == pugi::status_ok){
      pugi::xml_node objects = doc.child("gpx");
      for (pugi::xml_node object = objects.first_child(); object;
        object = object.next_sibling()) {
        if (!strcmp(object.name(), "rte")) {
          pRoute = GPXLoadRoute1(object, true, false, false, 0, true);
          // Check for duplicate GUID
          if (g_pRouteMan){
            bool b_add = true;
            Route *duplicate = g_pRouteMan->FindRouteByGUID(pRoute->GetGUID());
            if (duplicate){
              AcceptObjectDialog dialog2(NULL, wxID_ANY, _("OpenCPN Server Message"),
                "", wxDefaultPosition, wxDefaultSize, SYMBOL_STG_STYLE );

              dialog2.SetMessage("The received route route already exists on this system.\nReplace?");
              dialog2.SetCheck1Message(_("Always replace objects from this source?"));

              if (dialog2.ShowModal() != ID_STG_OK){
                b_add = false;
                return_stat = RESTServerResult::RESULT_DUPLICATE_REJECTED;
              }
              else{
                //  Remove the existing duplicate route before adding new route
                g_pRouteMan->DeleteRoute(duplicate);
              }
            }

            if (b_add)  {
              // And here is the payoff....

              // Add the route to the global list
              NavObjectCollection1 pSet;

              if (InsertRouteA(pRoute, &pSet))
                return_stat = RESTServerResult::RESULT_NO_ERROR;
              else
                return_stat = RESTServerResult::RESULT_ROUTE_INSERT_ERROR;
            }
          }
        }
      }
    }
  }
  else{
    return_stat = RESTServerResult::RESULT_OBJECT_REJECTED;

  }
#else
    // FIXME (leamas?)
    // What should the CLI app do here?
    return_stat = RESTServerResult::RESULT_GENERIC_ERROR;
#endif

  return_status = return_stat;

  std::lock_guard<std::mutex> lock{mx};
  return_status_condition.notify_one();
}


static const char *s_http_addr = "http://0.0.0.0:8000";    // HTTP port
static const char *s_https_addr = "https://0.0.0.0:8443";  // HTTPS port


// We use the same event handler function for HTTP and HTTPS connections
// fn_data is NULL for plain HTTP, and non-NULL for HTTPS
static void fn(struct mg_connection *c, int ev, void *ev_data, void *fn_data) {
  RESTServer *parent = static_cast<RESTServer *>(fn_data);

  if (ev == MG_EV_ACCEPT /*&& fn_data != NULL*/) {
     struct mg_tls_opts opts;
     memset(&opts, 0, sizeof(mg_tls_opts));

     opts.ca = NULL; //"cert.pem";         // Uncomment to enable two-way SSL
     opts.cert = parent->m_cert_file.c_str();       // Certificate PEM file
     opts.certkey = parent->m_key_file.c_str();     // The key PEM file
     opts.ciphers = NULL;
     mg_tls_init(c, &opts);
  } else if (ev == MG_EV_HTTP_MSG) {
    struct mg_http_message *hm = (struct mg_http_message *) ev_data;
    if (mg_http_match_uri(hm, "/api/rx_object")) {

      struct mg_str source = mg_http_var(hm->query, mg_str("source"));

      if(source.len && hm->body.len )
      {
        std::string xml_content(hm->body.ptr, hm->body.len);
        std::string source_peer(source.ptr, source.len);
        //printf("%s\n", xml_content.c_str());

       //std::ofstream b_stream("bodyfile",  std::fstream::out | std::fstream::binary);
       //b_stream.write(hm->body.ptr, hm->body.len);

        return_status = -1;

        if (parent){
          RESTServerEvent Nevent(wxEVT_RESTFUL_SERVER, 0);
          auto buffer = std::make_shared<std::string>(xml_content);
          Nevent.SetPayload(buffer);
          Nevent.SetSource(source_peer);
          parent->AddPendingEvent(Nevent);
        }

        std::unique_lock<std::mutex> lock{mx};
        while (return_status < 0) { // !predicate
          std::this_thread::sleep_for (std::chrono::milliseconds(100));
          return_status_condition.wait(lock);
        }
        lock.unlock();
      }

      mg_http_reply(c, 200, "", "{\"result\": %d}\n", return_status);
    }
  }
  (void) fn_data;
}

RESTServerThread::RESTServerThread(RESTServer* Launcher) {
  m_pParent = Launcher;  // This thread's immediate "parent"

  Create();
}

RESTServerThread::~RESTServerThread(void) {}

void RESTServerThread::OnExit(void) {}

void* RESTServerThread::Entry() {
  bool not_done = true;
  m_pParent->SetSecThreadActive();  // I am alive

  struct mg_mgr mgr;                            // Event manager
  mg_log_set(MG_LL_DEBUG);                      // Set log level
  mg_mgr_init(&mgr);                            // Initialise event manager
  mg_http_listen(&mgr, s_https_addr, fn, m_pParent);  // Create HTTP listener
  //mg_http_listen(&mgr, s_https_addr, fn, (void *) 1);  // HTTPS listener
  for (;;) mg_mgr_poll(&mgr, 1000);                    // Infinite event loop
  mg_mgr_free(&mgr);

thread_exit:
  m_pParent->SetSecThreadInActive();  // I am dead
  m_pParent->m_Thread_run_flag = -1;

  return 0;
}
