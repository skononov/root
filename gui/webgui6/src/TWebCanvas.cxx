// Author: Sergey Linev, GSI   7/12/2016

/*************************************************************************
 * Copyright (C) 1995-2018, Rene Brun and Fons Rademakers.               *
 * All rights reserved.                                                  *
 *                                                                       *
 * For the licensing terms see $ROOTSYS/LICENSE.                         *
 * For the list of contributors see $ROOTSYS/README/CREDITS.             *
 *************************************************************************/

#include "TWebCanvas.h"

#include "TWebSnapshot.h"
#include "TWebPadPainter.h"
#include "TWebMenuItem.h"

#include "TSystem.h"
#include "TCanvas.h"
#include "TROOT.h"
#include "TClass.h"
#include "TColor.h"
#include "TObjArray.h"
#include "TArrayI.h"
#include "TList.h"
#include "TH1.h"
#include "TGraph.h"
#include "TBufferJSON.h"
#include "Riostream.h"
#include "TBase64.h"

#include <ROOT/RWebWindowsManager.hxx>

#include <stdio.h>
#include <string.h>

TWebCanvas::TWebCanvas(TCanvas *c, const char *name, Int_t x, Int_t y, UInt_t width, UInt_t height)
   : TCanvasImp(c, name, x, y, width, height)
{
}

Int_t TWebCanvas::InitWindow()
{
   // at this place canvas is not yet register to the list of canvases - we cannot start browser
   return 111222333; // should not be used at all
}

////////////////////////////////////////////////////////////////////////////////
/// Creates web-based pad painter

TVirtualPadPainter *TWebCanvas::CreatePadPainter()
{
   return new TWebPadPainter();
}

////////////////////////////////////////////////////////////////////////////////
/// Returns kTRUE when object is fully supported on JSROOT side
/// In ROOT7 Paint function will just return appropriate flag that object can be displayed on JSROOT side

Bool_t TWebCanvas::IsJSSupportedClass(TObject *obj)
{
   if (!obj)
      return kTRUE;

   static const struct {
      const char *name;
      bool with_derived;
   } supported_classes[] = {{"TH1", true},
                            {"TF1", true},
                            {"TGraph", true},
                            {"TFrame", false},
                            {"THStack", false},
                            {"TMultiGraph", false},
                            {"TGraphPolargram", true},
                            {"TPave", true},
                            {"TGaxis", false},
                            {"TPave", true},
                            {"TArrow", false},
                            {"TBox", false},  // in principle, can be handled via TWebPainter
                            {"TWbox", false}, // some extra calls which cannout be handled via TWebPainter
                            {"TLine", false}, // also can be handler via TWebPainter
                            {"TText", false},
                            {"TLatex", false},
                            {"TMathText", false},
                            {"TMarker", false},
                            {"TPolyMarker3D", false},
                            {"TGraph2D", false},
                            {0, false}};

   // fast check of class name
   for (int i = 0; supported_classes[i].name != 0; ++i)
      if (strcmp(supported_classes[i].name, obj->ClassName()) == 0)
         return kTRUE;

   // now check inheritance only for configured classes
   for (int i = 0; supported_classes[i].name != 0; ++i)
      if (supported_classes[i].with_derived)
         if (obj->InheritsFrom(supported_classes[i].name))
            return kTRUE;

   return kFALSE;
}

//////////////////////////////////////////////////////////////////////////////////////////////////
/// search of object with given id in list of primitives
/// One could specify pad where search could be start
/// Also if object is in list of primitives, one could ask for entry link for such object,
/// This can allow to change draw option

TObject *TWebCanvas::FindPrimitive(const char *sid, TPad *pad, TObjLink **padlnk)
{

   if (!pad)
      pad = Canvas();

   const char *kind = "";
   const char *separ = strchr(sid, '#');
   UInt_t id = 0;

   if (separ == 0) {
      id = (UInt_t)TString(sid).Atoll();
   } else {
      kind = separ + 1;
      id = (UInt_t)TString(sid, separ - sid).Atoll();
   }

   if (TString::Hash(&pad, sizeof(pad)) == id)
      return pad;

   TObjLink *lnk = pad->GetListOfPrimitives()->FirstLink();
   while (lnk) {
      TObject *obj = lnk->GetObject();
      if (!obj) {
         lnk = lnk->Next();
         continue;
      }
      TH1 *h1 = obj->InheritsFrom(TH1::Class()) ? (TH1 *)obj : nullptr;
      if (TString::Hash(&obj, sizeof(obj)) == id) {
         if (h1 && (*kind == 'x'))
            return h1->GetXaxis();
         if (h1 && (*kind == 'y'))
            return h1->GetYaxis();
         if (h1 && (*kind == 'z'))
            return h1->GetZaxis();
         if (padlnk)
            *padlnk = lnk;
         return obj;
      }
      if (h1) {
         TIter fiter(h1->GetListOfFunctions());
         TObject *fobj = nullptr;
         while ((fobj = fiter()) != nullptr)
            if (TString::Hash(&fobj, sizeof(fobj)) == id)
               return fobj;
      } else if (obj->InheritsFrom(TPad::Class())) {
         obj = FindPrimitive(sid, (TPad *)obj);
         if (obj)
            return obj;
      }
      lnk = lnk->Next();
   }

   return nullptr;
}

//////////////////////////////////////////////////////////////////////////////////////////////////
/// Creates representation of the object for painting in web browser

TWebSnapshot *TWebCanvas::CreateObjectSnapshot(TObject *obj, const char *opt)
{
   TWebSnapshot *sub = new TWebSnapshot();
   sub->SetObjectIDAsPtr(obj);
   sub->SetOption(opt);
   TWebPainting *p = nullptr;

   if (!IsJSSupportedClass(obj)) {
      TWebPadPainter *painter = dynamic_cast<TWebPadPainter *>(Canvas()->GetCanvasPainter());
      if (!painter) {
         Error("CreateObjectSnapshot", "Not found WebPadPainter when paint class %s", obj->ClassName());
      } else {
         painter->ResetPainting();                                        // ensure painter is created
         painter->SetWebCanvasSize(Canvas()->GetWw(), Canvas()->GetWh()); // provide canvas dimension

         // calling Paint function for the object
         obj->Paint(opt);

         p = painter->TakePainting();
         fHasSpecials = kTRUE;
      }
   }

   // when paint method was used and produce output

   if (p) {
      p->FixSize();
      sub->SetSnapshot(TWebSnapshot::kSVG, p, kTRUE);
   } else {
      sub->SetSnapshot(TWebSnapshot::kObject, obj);
   }

   return sub;
}

//////////////////////////////////////////////////////////////////////////////////////////////////
/// Add special canvas objects like palette

Bool_t TWebCanvas::AddCanvasSpecials(TPadWebSnapshot *master)
{
   // if (!TColor::DefinedColors()) return 0;
   TObjArray *colors = (TObjArray *)gROOT->GetListOfColors();

   if (!colors)
      return kFALSE;
   Int_t cnt = 0;
   for (Int_t n = 0; n <= colors->GetLast(); ++n)
      if (colors->At(n) != nullptr)
         cnt++;
   if (cnt <= 598)
      return kFALSE; // normally there are 598 colors defined

   TWebSnapshot *sub = new TWebSnapshot();
   sub->SetSnapshot(TWebSnapshot::kSpecial, colors);
   master->Add(sub);

   if (gDebug > 1)
      Info("AddCanvasSpecials", "ADD COLORS TABLES %d", cnt);

   // save the current palette
   TArrayI pal = TColor::GetPalette();
   Int_t palsize = pal.GetSize();
   TObjArray *CurrentColorPalette = new TObjArray();
   CurrentColorPalette->SetName("CurrentColorPalette");
   for (Int_t i = 0; i < palsize; i++)
      CurrentColorPalette->Add(gROOT->GetColor(pal[i]));

   sub = new TWebSnapshot();
   sub->SetSnapshot(TWebSnapshot::kSpecial, CurrentColorPalette, kTRUE);
   master->Add(sub);

   return kTRUE;
}

//////////////////////////////////////////////////////////////////////////////////////////////////
/// Create snapshot for pad and all primitives

TString TWebCanvas::CreateSnapshot(TPad *pad, TPadWebSnapshot *master, TList *primitives_lst)
{
   TList master_lst; // main list of TList object which are primitives or functions
   if (!master && !primitives_lst)
      primitives_lst = &master_lst;

   TPadWebSnapshot *curr = new TPadWebSnapshot();
   curr->SetActive(pad == gPad);
   curr->SetObjectIDAsPtr(pad);
   curr->SetSnapshot(TWebSnapshot::kSubPad, pad);

   if (master)
      master->Add(curr);

   if (primitives_lst == &master_lst)
      AddCanvasSpecials(curr);

   TList *primitives = pad->GetListOfPrimitives();

   primitives_lst->Add(primitives); // add list of primitives

   TIter iter(primitives);
   TObject *obj = nullptr;
   while ((obj = iter()) != nullptr) {
      if (obj->InheritsFrom(TPad::Class())) {
         CreateSnapshot((TPad *)obj, curr, primitives_lst);
      } else if (obj->InheritsFrom(TH1::Class())) {
         TWebSnapshot *sub = new TWebSnapshot();
         TH1 *hist = (TH1 *)obj;
         sub->SetObjectIDAsPtr(hist);
         sub->SetOption(iter.GetOption());
         sub->SetSnapshot(TWebSnapshot::kObject, obj);
         curr->Add(sub);

         TIter fiter(hist->GetListOfFunctions());
         TObject *fobj = nullptr;
         while ((fobj = fiter()) != nullptr)
            if (!fobj->InheritsFrom("TPaveStats") && !fobj->InheritsFrom("TPaletteAxis"))
               curr->Add(CreateObjectSnapshot(fobj, fiter.GetOption()));

         primitives_lst->Add(hist->GetListOfFunctions());
      } else if (obj->InheritsFrom(TGraph::Class())) {
         TWebSnapshot *sub = new TWebSnapshot();
         TGraph *gr = (TGraph *)obj;
         sub->SetObjectIDAsPtr(gr);
         sub->SetOption(iter.GetOption());
         sub->SetSnapshot(TWebSnapshot::kObject, obj);
         curr->Add(sub);

         TIter fiter(gr->GetListOfFunctions());
         TObject *fobj = nullptr;
         while ((fobj = fiter()) != nullptr)
            if (!fobj->InheritsFrom("TPaveStats")) // stats should be created on the client side
               curr->Add(CreateObjectSnapshot(fobj, fiter.GetOption()));

         primitives_lst->Add(gr->GetListOfFunctions());
      } else {
         curr->Add(CreateObjectSnapshot(obj, iter.GetOption()));
      }
   }

   if (primitives_lst != &master_lst)
      return "";

   // now move all primitives and functions into separate list to perform I/O

   // TBufferJSON::ExportToFile("canvas.json", pad);

   TList save_lst;
   TIter diter(&master_lst);
   TList *dlst = nullptr;
   while ((dlst = (TList *)diter()) != nullptr) {
      TIter fiter(dlst);
      while ((obj = fiter()) != nullptr)
         save_lst.Add(obj, fiter.GetOption());
      save_lst.Add(dlst); // add list itslef to have marker
      dlst->Clear("nodelete");
   }

   TString res = TBufferJSON::ConvertToJSON(curr, 23);

   // TODO: this is only for debugging, remove it later
   // static int filecnt = 0;
   // TBufferJSON::ExportToFile(Form("snapshot_%d.json", (filecnt++) % 10), curr);

   delete curr; // destroy created snapshot

   TIter siter(&save_lst);
   diter.Reset();
   while ((dlst = (TList *)diter()) != nullptr) {
      while ((obj = siter()) != nullptr) {
         if (obj == dlst)
            break;
         dlst->Add(obj, siter.GetOption());
      }
   }

   save_lst.Clear("nodelete");

   master_lst.Clear("nodelete");

   return res;
}

void TWebCanvas::CheckDataToSend()
{
   if (!Canvas())
      return;

   for (auto &&conn: fWebConn) {

      // check if direct data sending is possible
      if (!fWindow->CanSend(conn.fConnId, true))
         continue;

      std::string buf;

      if (conn.fGetMenu.length() > 0) {

         TObject *obj = FindPrimitive(conn.fGetMenu.c_str());
         if (!obj)
            obj = Canvas();

         TWebMenuItems items;
         items.PopulateObjectMenu(obj, obj->IsA());
         buf = "MENU:";
         buf.append(conn.fGetMenu);
         buf.append(":");
         buf.append(items.ProduceJSON().Data());

         conn.fGetMenu.clear();
      } else if (conn.fDrawVersion < fCanvVersion) {
         buf = "SNAP6:";
         buf.append(std::to_string(fCanvVersion));
         buf.append(":");
         buf.append(CreateSnapshot(Canvas()).Data());

         // printf("Snapshot created %d\n", buf.Length());
         // if (buf.Length() < 10000) printf("Snapshot %s\n", buf.Data());
      } else if (!conn.fSend.empty()) {
         std::swap(buf, conn.fSend);
      }

      if (!buf.empty())
         fWindow->Send(conn.fConnId, buf);
   }
}

//////////////////////////////////////////////////////////////////////////////////////////
/// Close canvas (not implemented?)

void TWebCanvas::Close()
{
}

//////////////////////////////////////////////////////////////////////////////////////////
/// Create instance of RWebWindow to handle all kind of web connections
/// Returns URL string which can be used to access canvas locally

TString TWebCanvas::CreateWebWindow(int limit)
{
   if (!fWindow) {
      fWindow = ROOT::Experimental::RWebWindowsManager::Instance()->CreateWindow();

      fWindow->SetConnLimit(limit); // allow any number of connections

      fWindow->SetDefaultPage("file:$jsrootsys/files/canvas6.htm");

      fWindow->SetDataCallBack([this](unsigned connid, const std::string &arg) { ProcessData(connid, arg); });

      // fWindow->SetGeometry(500,300);
   }

   std::string url = fWindow->GetUrl(false);

   return TString(url.c_str());
}

//////////////////////////////////////////////////////////////////////////////////////////
/// Returns THttpServer instance, serving requests to the canvas

THttpServer *TWebCanvas::GetServer()
{
   if (!fWindow)
      return nullptr;

   return fWindow->GetServer();
}

//////////////////////////////////////////////////////////////////////////////////////////
/// Show canvas in specified place.
/// If parameter where not specified, default ROOT web display will be used

void TWebCanvas::ShowWebWindow(const std::string &where)
{
   if (fWindow)
      fWindow->Show(where);
}

//////////////////////////////////////////////////////////////////////////////////////////
/// Show canvas in browser window

void TWebCanvas::Show()
{
   CreateWebWindow();

   fWaitNewConnection = kTRUE;

   ShowWebWindow();
}

//////////////////////////////////////////////////////////////////////////////////////////
/// Function used to send command to browser to toggle menu, toolbar, editors, ...

void TWebCanvas::ShowCmd(const char *arg, Bool_t show)
{
   for (auto &&conn : fWebConn) {
      if (!conn.fConnId)
         continue;

      if (!conn.fSend.empty())
         Warning("ShowCmd", "Send operation not empty when try show %s", arg);

      conn.fSend = Form("SHOW:%s:%d", arg, show ? 1 : 0);
   }

   CheckDataToSend();
}

//////////////////////////////////////////////////////////////////////////////////////////
/// Activate object in editor in web browser

void TWebCanvas::ActivateInEditor(TPad *pad, TObject *obj)
{
   if (!pad || !obj) return;

   for (auto &&conn: fWebConn) {
      if (!conn.fConnId)
         continue;

      if (!conn.fSend.empty())
         Warning("ActivateInEditor", "Send operation not empty");

      UInt_t hash = TString::Hash(&obj, sizeof(obj));

      conn.fSend = Form("EDIT:%u", (unsigned) hash);
   }

   CheckDataToSend();
}

Bool_t TWebCanvas::HasEditor() const
{
   return (fClientBits & TCanvas::kShowEditor) != 0;
}

Bool_t TWebCanvas::HasMenuBar() const
{
   return (fClientBits & TCanvas::kMenuBar) != 0;
}

Bool_t TWebCanvas::HasStatusBar() const
{
   return (fClientBits & TCanvas::kShowEventStatus) != 0;
}

Bool_t TWebCanvas::HasToolTips() const
{
   return (fClientBits & TCanvas::kShowToolTips) != 0;
}

void TWebCanvas::AssignStatusBits(UInt_t bits)
{
   fClientBits = bits;
   Canvas()->SetBit(TCanvas::kShowEventStatus, bits & TCanvas::kShowEventStatus);
   Canvas()->SetBit(TCanvas::kShowEditor, bits & TCanvas::kShowEditor);
   Canvas()->SetBit(TCanvas::kShowToolTips, bits & TCanvas::kShowToolTips);
   Canvas()->SetBit(TCanvas::kMenuBar, bits & TCanvas::kMenuBar);
}

//////////////////////////////////////////////////////////////////////////////////////////
/// Extract information for current visible range and set to correspondent pad object

Bool_t TWebCanvas::DecodeAllRanges(const char *arg)
{
   if (!arg || !*arg)
      return kFALSE;

   std::vector<TWebPadRange> *arr = nullptr;

   TBufferJSON::FromJSON(arr, arg);

   if (!arr)
      return kFALSE;

   for (unsigned n = 0; n < arr->size(); ++n) {
      TWebPadRange &r = arr->at(n);
      TPad *pad = dynamic_cast<TPad *>(FindPrimitive(r.snapid.c_str()));

      if (!pad)
         continue;

      if (pad == Canvas()) AssignStatusBits(r.bits);

      if (r.active && (pad != gPad)) gPad = pad;

      pad->SetTicks(r.tickx, r.ticky);
      pad->SetGrid(r.gridx, r.gridy);
      pad->SetLogx(r.logx);
      pad->SetLogy(r.logy);
      pad->SetLogz(r.logz);

      pad->SetLeftMargin(r.mleft);
      pad->SetRightMargin(r.mright);
      pad->SetTopMargin(r.mtop);
      pad->SetBottomMargin(r.mbottom);

      for (unsigned k = 0; k < r.primitives.size(); ++k) {
         TObjLink *lnk = nullptr;
         TObject *obj = FindPrimitive(r.primitives[k].snapid.c_str(), pad, &lnk);
         if (obj && lnk) {
            if (gDebug > 1)
               Info("DecodeAllRanges", "Set draw option \"%s\" for object %s %s", r.primitives[k].opt.c_str(),
                    obj->ClassName(), obj->GetName());
            lnk->SetOption(r.primitives[k].opt.c_str());
         }
      }

      if (!r.ranges) continue;

      Double_t ux1_, ux2_, uy1_, uy2_, px1_, px2_, py1_, py2_;

      pad->GetRange(px1_, py1_, px2_, py2_);
      pad->GetRangeAxis(ux1_, uy1_, ux2_, uy2_);

      if ((r.ux1 == ux1_) && (r.ux2 == ux2_) && (r.uy1 == uy1_) && (r.uy2 == uy2_) && (r.px1 == px1_) &&
          (r.px2 == px2_) && (r.py1 == py1_) && (r.py2 == py2_))
         continue; // no changes

      pad->Range(r.px1, r.py1, r.px2, r.py2);
      pad->RangeAxis(r.ux1, r.uy1, r.ux2, r.uy2);

      if (gDebug > 1)
         Info("DecodeAllRanges", "Change ranges for pad %s", pad->GetName());

      // without special objects no need for explicit update of the canvas
      if (fHasSpecials)
         pad->Modified(kTRUE);
   }

   delete arr;

   if (fUpdatedSignal) fUpdatedSignal(); // invoke signal

   return kTRUE;
}

//////////////////////////////////////////////////////////////////////////////////////////
/// Handle data from web browser

void TWebCanvas::ProcessData(unsigned connid, const std::string &arg)
{
   if (arg.empty())
      return;

   if (arg == "CONN_READY") {

      fWebConn.emplace_back(connid);

      CheckDataToSend();

      fWaitNewConnection = kFALSE; // established, can be reset

      return;
   }

   // try to identify connection for given WS request
   WebConn *conn(nullptr);
   bool is_first = true;
   auto iter = fWebConn.begin();
   while (iter != fWebConn.end()) {
      if (iter->fConnId == connid) {
         conn = &(*iter);
         break;
      }
      ++iter;
      is_first = false;
   }

   if (!conn)
      return;

   const char *cdata = arg.c_str();

   if (arg == "CONN_CLOSED") {

      fWebConn.erase(iter);

   } else if (arg == "KEEPALIVE") {
      // do nothing

   } else if (arg == "QUIT") {

      // use window manager to correctly terminate http server
      ROOT::Experimental::RWebWindowsManager::Instance()->Terminate();

   } else if (strncmp(cdata, "READY6:", 7) == 0) {

      // this is reply on drawing of ROOT6 snapshot
      // it confirms when drawing of specific canvas version is completed
      cdata += 7;

      const char *separ = strchr(cdata, ':');
      if (!separ) {
         conn->fDrawVersion = TString(cdata).Atoll();
      } else {
         conn->fDrawVersion = TString(cdata, separ - cdata).Atoll();
         cdata = separ + 1;
         if ((gDebug > 1) && is_first)
            Info("ProcessData", "RANGES %s", cdata);
         if (is_first)
            DecodeAllRanges(cdata); // only first connection can set ranges
      }
      CheckDataToSend();

   } else if (strncmp(cdata, "RANGES6:", 8) == 0) {

      if (is_first) // only first connection can set ranges
         DecodeAllRanges(cdata + 8);

   } else if (strncmp(cdata, "STATUSBITS:", 11) == 0) {

      if (is_first) { // only first connection can set ranges
         AssignStatusBits((unsigned) TString(cdata + 11).Atoi());
         if (fUpdatedSignal) fUpdatedSignal(); // invoke signal
      }

   } else if (strncmp(cdata, "GETMENU:", 8) == 0) {

      conn->fGetMenu = cdata + 8;
      CheckDataToSend();

   } else if (strncmp(cdata, "OBJEXEC:", 8) == 0) {

      TString buf(cdata + 8);
      Int_t pos = buf.First(':');

      if ((pos > 0) && is_first) { // only first client can execute commands
         TString sid(buf, pos);
         buf.Remove(0, pos + 1);

         TObject *obj = FindPrimitive(sid.Data());
         if (obj && (buf.Length() > 0)) {
            TString exec;
            exec.Form("((%s*) %p)->%s;", obj->ClassName(), obj, buf.Data());
            Info("ProcessWS", "Obj %s Execute %s", obj->GetName(), exec.Data());
            gROOT->ProcessLine(exec);

            // PerformUpdate(); // check that canvas was changed
            if (IsAnyPadModified(Canvas()))
               fCanvVersion++;
            CheckDataToSend();
         }
      }

   } else if (strncmp(cdata, "EXECANDSEND:", 12) == 0) {

      TString buf(cdata + 12), reply;
      TObject *obj = nullptr;

      Int_t pos = buf.First(':');

      if ((pos > 0) && is_first) { // only first client can execute commands
         reply.Append(buf, pos);
         buf.Remove(0, pos + 1);
         pos = buf.First(':');
         if (pos > 0) {
            TString sid(buf, pos);
            buf.Remove(0, pos + 1);
            obj = FindPrimitive(sid.Data());
         }
      }

      if (obj && (buf.Length() > 0) && (reply.Length() > 0)) {
         TString exec;
         exec.Form("((%s*) %p)->%s;", obj->ClassName(), obj, buf.Data());
         if (gDebug > 1)
            Info("ProcessData", "Obj %s Exec %s", obj->GetName(), exec.Data());

         Long_t res = gROOT->ProcessLine(exec);
         TObject *resobj = (TObject *)res;
         if (resobj) {
            conn->fSend = reply.Data();
            conn->fSend.append(":");
            conn->fSend.append(TBufferJSON::ConvertToJSON(resobj, 23).Data());
            if (reply[0] == 'D')
               delete resobj; // delete object if first symbol in reply is D
         }

         CheckDataToSend(); // check if data should be send
      }

   } else if (strncmp(cdata, "RELOAD", 6) == 0) {

      conn->fDrawVersion = 0;
      CheckDataToSend();

   } else if (strncmp(cdata, "SAVE:", 5) == 0) {

      const char *img = cdata + 5;

      const char *separ = strchr(img, ':');
      if (separ) {
         TString filename(img, separ - img);
         img = separ + 1;

         std::ofstream ofs(filename.Data());

         if (filename.Index(".svg") != kNPOS) {
            // ofs << "<?xml version=\"1.0\" standalone=\"no\"?>";
            ofs << img;
         } else {
            TString binary = TBase64::Decode(img);
            ofs.write(binary.Data(), binary.Length());
         }
         ofs.close();

         Info("ProcessWS", "File %s has been created", filename.Data());
      }
      CheckDataToSend();

   } else if (strncmp(cdata, "PRODUCE:", 8) == 0) {

      Canvas()->Print(cdata+8);

      CheckDataToSend();

   } else if (strncmp(cdata, "PADCLICKED:", 11) == 0) {

      TWebPadClick *click = nullptr;

      // only from the first client analyze pad click events
      if (is_first)
         TBufferJSON::FromJSON(click, cdata + 11);

      if (click) {

         TPad *pad = dynamic_cast<TPad*> (FindPrimitive(click->padid.c_str()));
         if (pad && (pad != gPad)) {
            Info("ProcessWS", "Activate pad %s", pad->GetName());
            gPad = pad;
            Canvas()->SetClickSelectedPad(pad);
            if (fActivePadChangedSignal) fActivePadChangedSignal(pad);
         }

         if (!click->objid.empty()) {
            TObject *selobj = FindPrimitive(click->objid.c_str());
            Canvas()->SetClickSelected(selobj);
            if (pad && selobj && fObjSelectSignal) fObjSelectSignal(pad, selobj);
         }

         if ((click->x >= 0) && (click->y >= 0)) {
            if (click->dbl && fPadDblClickedSignal)
               fPadDblClickedSignal(pad, click->x, click->y);
            else if (fPadClickedSignal)
               fPadClickedSignal(pad, click->x, click->y);
         }

         delete click; // do not forget to destroy
      }

   } else {
      Error("ProcessWS", "GET unknown request %d %30s", (int)arg.length(), cdata);
   }
}

//////////////////////////////////////////////////////////////////////////////////////////
/// returns true when any pad or sub pad modified
/// reset modified flags

Bool_t TWebCanvas::IsAnyPadModified(TPad *pad)
{
   Bool_t res = kFALSE;

   if (pad->IsModified()) {
      pad->Modified(kFALSE);
      res = kTRUE;
   }

   TIter iter(pad->GetListOfPrimitives());
   TObject *obj = 0;
   while ((obj = iter()) != 0) {
      if (obj->InheritsFrom(TPad::Class()) && IsAnyPadModified((TPad *)obj))
         res = kTRUE;
   }

   return res;
}

UInt_t TWebCanvas::GetWindowGeometry(Int_t &x, Int_t &y, UInt_t &w, UInt_t &h)
{
   // reset dimension in gVirtualX  - it will be requested immediately
   x = 0;
   y = 0;
   w = Canvas()->GetWw() + 4;
   h = Canvas()->GetWh() + 28;
   return 0;
}

//////////////////////////////////////////////////////////////////////////////////////////
/// if canvas or any subpad was modified,
/// scan all primitives in the TCanvas and subpads and convert them into
/// the structure which will be delivered to JSROOT client

Bool_t TWebCanvas::PerformUpdate()
{
   if (IsAnyPadModified(Canvas()))
      fCanvVersion++;

   CheckDataToSend();

   // block in canvas update, can it be optional
   WaitWhenCanvasPainted(fCanvVersion);

   return kTRUE;
}

//////////////////////////////////////////////////////////////////////////////////////////
/// Wait when specified version of canvas was painted and confirmed by browser

Bool_t TWebCanvas::WaitWhenCanvasPainted(Long64_t ver)
{
   // simple polling loop until specified version delivered to the clients

   long cnt = 0;

   if (gDebug > 2)
      Info("WaitWhenCanvasPainted", "version %ld", (long)ver);

   while (cnt++ < 1000) {

      if (!fWindow->HasConnection(0, false)) {
         if (gDebug > 2)
            Info("WaitWhenCanvasPainted", "no connections - abort");
         return kFALSE; // wait ~1 min if no new connection established
      }

      if ((fWebConn.size() > 0) && (fWebConn.front().fDrawVersion >= ver)) {
         if (gDebug > 2)
            Info("WaitWhenCanvasPainted", "ver %ld got painted", (long)ver);
         return kTRUE;
      }

      gSystem->ProcessEvents();

      gSystem->Sleep((cnt < 500) ? 1 : 100); // increase sleep interval when do very often
   }

   if (gDebug > 2)
      Info("WaitWhenCanvasPainted", "timeout");

   return kFALSE;
}
