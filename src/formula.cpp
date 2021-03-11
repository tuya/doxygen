/******************************************************************************
 *
 * Copyright (C) 1997-2020 by Dimitri van Heesch.
 *
 * Permission to use, copy, modify, and distribute this software and its
 * documentation under the terms of the GNU General Public License is hereby
 * granted. No representations are made about the suitability of this software
 * for any purpose. It is provided "as is" without express or implied warranty.
 * See the GNU General Public License for more details.
 *
 * Documents produced by Doxygen are derivative works derived from the
 * input used in their production; they are not affected by this license.
 *
 */

#include "formula.h"
#include "message.h"
#include "config.h"
#include "ftextstream.h"
#include "util.h"
#include "portable.h"
#include "image.h"
#include "fileinfo.h"
#include "dir.h"

#include <qfile.h>

#include <map>
#include <vector>
#include <string>
#include <utility>
#include <fstream>

// TODO: remove these dependencies
#include "doxygen.h" // for Doxygen::indexList
#include "index.h"   // for Doxygen::indexList

static int determineInkscapeVersion(Dir &thisDir);

// Remove the temporary files
#define RM_TMP_FILES (true)
//#define RM_TMP_FILES (false)

struct FormulaManager::Private
{
  void storeDisplaySize(int id,int w,int h)
  {
    displaySizeMap.insert(std::make_pair(id,DisplaySize(w,h)));
  }
  DisplaySize getDisplaySize(int id)
  {
    auto it = displaySizeMap.find(id);
    if (it!=displaySizeMap.end())
    {
      return it->second;
    }
    return DisplaySize(-1,-1);
  }
  StringVector  formulas;
  IntMap formulaMap;
  std::map<int,DisplaySize> displaySizeMap;
};

FormulaManager::FormulaManager() : p(new Private)
{
}

FormulaManager &FormulaManager::instance()
{
  static FormulaManager fm;
  return fm;
}

void FormulaManager::readFormulas(const char *dir,bool doCompare)
{
  std::ifstream f(std::string(dir)+"/formula.repository");
  if (f.is_open())
  {
    uint formulaCount=0;
    msg("Reading formula repository...\n");
    std::string line;
    int lineNr=1;
    while (getline(f,line))
    {
      // format: \_form#<digits>=<digits>x<digits>:formula
      size_t hi=line.find('#');
      size_t ei=line.find('=');
      size_t se=line.find(':'); // find name and text separator.
      if (ei==std::string::npos || hi==std::string::npos || se==std::string::npos || hi>se || ei<hi || ei>se)
      {
        warn_uncond("%s/formula.repository is corrupted at line %d!\n",dir,lineNr);
        break;
      }
      else
      {
        std::string formName = line.substr(0,se); // '\_form#<digits>=<digits>x<digits>' part
        std::string formText = line.substr(se+1); // 'formula' part
        int w=-1,h=-1;
        size_t xi=formName.find('x',ei);
        if (xi!=std::string::npos)
        {
          w=std::stoi(formName.substr(ei+1,xi-ei-1)); // digits from '=<digits>x' part as int
          h=std::stoi(formName.substr(xi+1));         // digits from 'x<digits>' part as int
        }
        formName = formName.substr(0,ei); // keep only the '\_form#<digits>' part
        if (doCompare)
        {
          int formId = stoi(formName.substr(hi+1));
          std::string storedFormText = FormulaManager::instance().findFormula(formId);
          if (storedFormText!=formText)
          {
            term("discrepancy between formula repositories! Remove "
                "formula.repository and from_* files from output directories.\n");
          }
          formulaCount++;
        }
        int id = addFormula(formText);
        if (w!=-1 && h!=-1)
        {
          p->storeDisplaySize(id,w,h);
        }
      }
      lineNr++;
    }
    if (doCompare && formulaCount!=p->formulas.size())
    {
      term("size discrepancy between formula repositories! Remove "
           "formula.repository and from_* files from output directories.\n");
    }
  }
}

void FormulaManager::generateImages(const char *path,Format format,HighDPI hd) const
{
  Dir d(path);
  // store the original directory
  if (!d.exists())
  {
    term("Output directory '%s' does not exist!\n",path);
  }
  std::string oldDir = Dir::currentDirPath();
  QCString macroFile = Config_getString(FORMULA_MACROFILE);
  QCString stripMacroFile;
  if (!macroFile.isEmpty())
  {
    FileInfo fi(macroFile.str());
    macroFile=fi.absFilePath();
    stripMacroFile = fi.fileName();
  }

  // go to the html output directory (i.e. path)
  Dir::setCurrent(d.absPath());
  Dir thisDir;
  // generate a latex file containing one formula per page.
  QCString texName="_formulas.tex";
  IntVector formulasToGenerate;
  QFile f(texName);
  if (f.open(IO_WriteOnly))
  {
    FTextStream t(&f);
    if (Config_getBool(LATEX_BATCHMODE)) t << "\\batchmode" << endl;
    t << "\\documentclass{article}" << endl;
    t << "\\usepackage{ifthen}" << endl;
    t << "\\usepackage{epsfig}" << endl; // for those who want to include images
    t << "\\usepackage[utf8]{inputenc}" << endl; // looks like some older distributions with newunicode package 1.1 need this option.
    writeExtraLatexPackages(t);
    writeLatexSpecialFormulaChars(t);
    if (!macroFile.isEmpty())
    {
      copyFile(macroFile,stripMacroFile);
      t << "\\input{" << stripMacroFile << "}" << endl;
    }
    t << "\\pagestyle{empty}" << endl;
    t << "\\begin{document}" << endl;
    for (int i=0; i<(int)p->formulas.size(); i++)
    {
      QCString resultName;
      resultName.sprintf("form_%d.%s",i,format==Format::Vector?"svg":"png");
      // only formulas for which no image exists are generated
      FileInfo fi(resultName.str());
      if (!fi.exists())
      {
        // we force a pagebreak after each formula
        t << p->formulas[i].c_str() << endl << "\\pagebreak\n\n";
        formulasToGenerate.push_back(i);
      }
      Doxygen::indexList->addImageFile(resultName);
    }
    t << "\\end{document}" << endl;
    f.close();
  }
  if (!formulasToGenerate.empty()) // there are new formulas
  {
    //printf("Running latex...\n");
    //system("latex _formulas.tex </dev/null >/dev/null");
    QCString latexCmd = "latex";
    Portable::sysTimerStart();
    char args[4096];
    sprintf(args,"-interaction=batchmode _formulas.tex >%s",Portable::devNull());
    if (Portable::system(latexCmd,args)!=0)
    {
      err("Problems running latex. Check your installation or look "
          "for typos in _formulas.tex and check _formulas.log!\n");
      Portable::sysTimerStop();
      Dir::setCurrent(oldDir);
      return;
    }
    Portable::sysTimerStop();
    //printf("Running dvips...\n");
    int pageIndex=1;
    for (int pageNum : formulasToGenerate)
    {
      msg("Generating image form_%d.%s for formula\n",pageNum,(format==Format::Vector) ? "svg" : "png");
      QCString formBase;
      formBase.sprintf("_form%d",pageNum);
      // run dvips to convert the page with number pageIndex to an
      // postscript file.
      sprintf(args,"-q -D 600 -n 1 -p %d -o %s_tmp.ps _formulas.dvi",
          pageIndex,formBase.data());
      Portable::sysTimerStart();
      if (Portable::system("dvips",args)!=0)
      {
        err("Problems running dvips. Check your installation!\n");
        Portable::sysTimerStop();
        Dir::setCurrent(oldDir);
        return;
      }
      Portable::sysTimerStop();

      // extract the bounding box for the postscript file
      sprintf(args,"-q -dBATCH -dNOPAUSE -P- -dNOSAFER -sDEVICE=bbox %s_tmp.ps 2>%s_tmp.epsi",
          formBase.data(),formBase.data());
      Portable::sysTimerStart();
      if (Portable::system(Portable::ghostScriptCommand(),args)!=0)
      {
        err("Problems running %s. Check your installation!\n",Portable::ghostScriptCommand());
        Portable::sysTimerStop();
        Dir::setCurrent(oldDir);
        return;
      }
      Portable::sysTimerStop();

      // extract the bounding box info from the generate .epsi file
      int x1=0,y1=0,x2=0,y2=0;
      FileInfo fi((formBase+"_tmp.epsi").str());
      if (fi.exists())
      {
        QCString eps = fileToString(formBase+"_tmp.epsi");
        int i = eps.find("%%BoundingBox:");
        if (i!=-1)
        {
          sscanf(eps.data()+i,"%%%%BoundingBox:%d %d %d %d",&x1,&y1,&x2,&y2);
        }
        else
        {
          err("Couldn't extract bounding box from %s_tmp.epsi",formBase.data());
        }
      }
      //printf("Bounding box [%d %d %d %d]\n",x1,y1,x2,y2);

      // convert the corrected EPS to a bitmap
      double scaleFactor = 1.25;
      int zoomFactor = Config_getInt(FORMULA_FONTSIZE);
      if (zoomFactor<8 || zoomFactor>50) zoomFactor=10;
      scaleFactor *= zoomFactor/10.0;

      int width  = (int)((x2-x1)*scaleFactor+0.5);
      int height = (int)((y2-y1)*scaleFactor+0.5);
      p->storeDisplaySize(pageNum,width,height);

      if (format==Format::Vector)
      {
        // crop the image to its bounding box
        sprintf(args,"-q -dBATCH -dNOPAUSE -P- -dNOSAFER -sDEVICE=pdfwrite"
                     " -o %s_tmp.pdf -c \"[/CropBox [%d %d %d %d] /PAGES pdfmark\" -f %s_tmp.ps",
                     formBase.data(),x1,y1,x2,y2,formBase.data());
        Portable::sysTimerStart();
        if (Portable::system(Portable::ghostScriptCommand(),args)!=0)
        {
          err("Problems running %s. Check your installation!\n",Portable::ghostScriptCommand());
          Portable::sysTimerStop();
          Dir::setCurrent(oldDir);
          return;
        }
        Portable::sysTimerStop();

        // if we have pdf2svg available use it to create a SVG image
        if (Portable::checkForExecutable("pdf2svg"))
        {
          sprintf(args,"%s_tmp.pdf form_%d.svg",formBase.data(),pageNum);
          Portable::sysTimerStart();
          if (Portable::system("pdf2svg",args)!=0)
          {
            err("Problems running pdf2svg. Check your installation!\n");
            Portable::sysTimerStop();
            Dir::setCurrent(oldDir);
            return;
          }
          Portable::sysTimerStop();
        }
        else if (Portable::checkForExecutable("inkscape")) // alternative is to use inkscape
        {
          int inkscapeVersion = determineInkscapeVersion(thisDir);
          if (inkscapeVersion == -1)
          {
            err("Problems determining the version of inkscape. Check your installation!\n");
            Dir::setCurrent(oldDir);
            return;
          }
          else if (inkscapeVersion == 0)
          {
            sprintf(args,"-l form_%d.svg -z %s_tmp.pdf 2>%s",pageNum,formBase.data(),Portable::devNull());
          }
          else // inkscapeVersion >= 1
          {
            sprintf(args,"--export-type=svg --export-filename=form_%d.svg %s_tmp.pdf 2>%s",pageNum,formBase.data(),Portable::devNull());
          }
          Portable::sysTimerStart();
          if (Portable::system("inkscape",args)!=0)
          {
            err("Problems running inkscape. Check your installation!\n");
            Portable::sysTimerStop();
            Dir::setCurrent(oldDir);
            return;
          }
          Portable::sysTimerStop();
        }
        else
        {
          err("Neither 'pdf2svg' nor 'inkscape' present for conversion of formula to 'svg'\n");
          return;
        }

        if (RM_TMP_FILES)
        {
          thisDir.remove(formBase.str()+"_tmp.pdf");
        }
      }
      else // format==Format::Bitmap
      {
        // crop the image to its bounding box
        sprintf(args,"-q -dBATCH -dNOPAUSE -P- -dNOSAFER -sDEVICE=eps2write"
                     " -o %s_tmp.eps -f %s_tmp.ps",formBase.data(),formBase.data());
        Portable::sysTimerStart();
        if (Portable::system(Portable::ghostScriptCommand(),args)!=0)
        {
          err("Problems running %s. Check your installation!\n",Portable::ghostScriptCommand());
          Portable::sysTimerStop();
          Dir::setCurrent(oldDir);
          return;
        }

        // read back %s_tmp.eps and replace
        // bounding box values with x1,y1,x2,y2 and remove the HiResBoundingBox
        QFile epsIn(formBase+"_tmp.eps");
        QFile epsOut(formBase+"_tmp_corr.eps");
        if (epsIn.open(IO_ReadOnly) && epsOut.open(IO_WriteOnly))
        {
          int maxLineLen=100*1024;
          while (!epsIn.atEnd())
          {
            QCString buf(maxLineLen);
            FTextStream t(&epsOut);
            int numBytes = epsIn.readLine(buf.rawData(),maxLineLen);
            if (numBytes>0)
            {
              buf.resize(numBytes+1);
              if (buf.startsWith("%%BoundingBox"))
              {
                t << "%%BoundingBox: " << x1 << " " << y1 << " " << x2 << " " << y2 << endl;
              }
              else if (buf.startsWith("%%HiResBoundingBox")) // skip this one
              {
              }
              else
              {
                t << buf;
              }
            }
          }
          epsIn.close();
          epsOut.close();
        }
        else
        {
          err("Problems correcting the eps files from %s_tmp.eps to %s_tmp_corr.eps\n",
              formBase.data(),formBase.data());
          Dir::setCurrent(oldDir);
          return;
        }

        if (hd==HighDPI::On) // for high DPI display it looks much better if the
                             // image resolution is higher than the display resolution
        {
          scaleFactor*=2;
        }

        Portable::sysTimerStop();
        sprintf(args,"-q -dNOSAFER -dBATCH -dNOPAUSE -dEPSCrop -sDEVICE=pnggray -dGraphicsAlphaBits=4 -dTextAlphaBits=4 "
            "-r%d -sOutputFile=form_%d.png %s_tmp_corr.eps",(int)(scaleFactor*72),pageNum,formBase.data());
        Portable::sysTimerStart();
        if (Portable::system(Portable::ghostScriptCommand(),args)!=0)
        {
          err("Problems running %s. Check your installation!\n",Portable::ghostScriptCommand());
          Portable::sysTimerStop();
          Dir::setCurrent(oldDir);
          return;
        }
        Portable::sysTimerStop();

        if (RM_TMP_FILES)
        {
          thisDir.remove(formBase.str()+"_tmp.eps");
          thisDir.remove(formBase.str()+"_tmp_corr.eps");
        }
      }

      // remove intermediate image files
      if (RM_TMP_FILES)
      {
        thisDir.remove(formBase.str()+"_tmp.ps");
        thisDir.remove(formBase.str()+"_tmp.epsi");
      }
      pageIndex++;
    }
    // remove intermediate files produced by latex
    if (RM_TMP_FILES)
    {
      thisDir.remove("_formulas.dvi");
      thisDir.remove("_formulas.log"); // keep file in case of errors
      thisDir.remove("_formulas.aux");
    }
  }
  // remove the latex file itself
  if (RM_TMP_FILES) thisDir.remove("_formulas.tex");

  // write/update the formula repository so we know what text the
  // generated images represent (we use this next time to avoid regeneration
  // of the images, and to avoid forcing the user to delete all images in order
  // to let a browser refresh the images).
  f.setName("formula.repository");
  if (f.open(IO_WriteOnly))
  {
    FTextStream t(&f);
    for (int i=0; i<(int)p->formulas.size(); i++)
    {
      DisplaySize size = p->getDisplaySize(i);
      t << "\\_form#" << i;
      if (size.width!=-1 && size.height!=-1)
      {
        t << "=" << size.width << "x" << size.height;
      }
      t << ":" << p->formulas[i].c_str() << endl;
    }
    f.close();
  }
  // reset the directory to the original location.
  Dir::setCurrent(oldDir);
}

void FormulaManager::clear()
{
  p->formulas.clear();
  p->formulaMap.clear();
}

int FormulaManager::addFormula(const std::string &formulaText)
{
  auto it = p->formulaMap.find(formulaText);
  if (it!=p->formulaMap.end()) // already stored
  {
    return it->second;
  }
  // store new formula
  int id = (int)p->formulas.size();
  p->formulaMap.insert(std::pair<std::string,int>(formulaText,id));
  p->formulas.push_back(formulaText);
  return id;
}

std::string FormulaManager::findFormula(int formulaId) const
{
  if (formulaId>=0 && formulaId<(int)p->formulas.size())
  {
    return p->formulas[formulaId];
  }
  return std::string();
}

bool FormulaManager::hasFormulas() const
{
  return !p->formulas.empty();
}

FormulaManager::DisplaySize FormulaManager::displaySize(int formulaId) const
{
  return p->getDisplaySize(formulaId);
}

// helper function to detect and return the major version of inkscape.
// return -1 if the version cannot be determined.
static int determineInkscapeVersion(Dir &thisDir)
{
  // The command line interface (CLI) of Inkscape 1.0 has changed in comparison to
  // previous versions. In order to invokine Inkscape, the used version is detected
  // and based on the version the right syntax of the CLI is chosen.
  static int inkscapeVersion = -2;
  if (inkscapeVersion == -2) // initial one time version check
  {
    QCString inkscapeVersionFile = "inkscape_version" ;
    inkscapeVersion = -1;
    QCString args = "-z --version >"+inkscapeVersionFile+" 2>"+Portable::devNull();
    Portable::sysTimerStart();
    if (Portable::system("inkscape",args)!=0)
    {
      // looks like the old syntax gave problems, lets try the new syntax
      args = " --version >"+inkscapeVersionFile+" 2>"+Portable::devNull();
      if (Portable::system("inkscape",args)!=0)
      {
        Portable::sysTimerStop();
        return -1;
      }
    }
    // read version file and determine major version
    QFile inkscapeVersionIn(inkscapeVersionFile);
    if (inkscapeVersionIn.open(IO_ReadOnly))
    {
      int maxLineLen=1024;
      while (!inkscapeVersionIn.atEnd())
      {
        QCString buf(maxLineLen);
        int numBytes = inkscapeVersionIn.readLine(buf.rawData(),maxLineLen);
        if (numBytes>0)
        {
          buf.resize(numBytes+1);
          int dotPos = buf.find('.');
          if (buf.startsWith("Inkscape ") && dotPos>0)
          {
            // get major version
            bool ok;
            int version = buf.mid(9,dotPos-9).toInt(&ok);
            if (!ok)
            {
              Portable::sysTimerStop();
              return -1;
            }
            inkscapeVersion = version;
            break;
          }
        }
        else
        {
          Portable::sysTimerStop();
          return -1;
        }
      }
      inkscapeVersionIn.close();
    }
    else // failed to open version file
    {
      Portable::sysTimerStop();
      return -1;
    }
    if (RM_TMP_FILES)
    {
      thisDir.remove(inkscapeVersionFile.str());
    }
    Portable::sysTimerStop();
  }
  return inkscapeVersion;
}
