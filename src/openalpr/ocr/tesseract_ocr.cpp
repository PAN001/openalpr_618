/*
 * Copyright (c) 2015 OpenALPR Technology, Inc.
 * Open source Automated License Plate Recognition [http://www.openalpr.com]
 *
 * This file is part of OpenALPR.
 *
 * OpenALPR is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License
 * version 3 as published by the Free Software Foundation
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
*/

#include "tesseract_ocr.h"
#include "config.h"
#include <omp.h>

#include "segmentation/charactersegmenter.h"

using namespace std;
using namespace cv;
using namespace tesseract;

namespace alpr
{

  TesseractOcr::TesseractOcr(Config* config)
  : OCR(config)
  {
    const string MINIMUM_TESSERACT_VERSION = "3.03";

    timespec s, e;
    getTimeMonotonic(&s);

    this->postProcessor.setConfidenceThreshold(config->postProcessMinConfidence, config->postProcessConfidenceSkipLevel);
    getTimeMonotonic(&e);

    cout << "OpenALPR setConfidenceThreshold Time: " << diffclock(s, e) << "ms." << endl;




    if (cmpVersion(tesseract.Version(), MINIMUM_TESSERACT_VERSION.c_str()) < 0)
    {
      std::cerr << "Warning: You are running an unsupported version of Tesseract." << endl;
      std::cerr << "Expecting at least " << MINIMUM_TESSERACT_VERSION << ", your version is: " << tesseract.Version() << endl;
    }


    getTimeMonotonic(&s);
    string TessdataPrefix = config->getTessdataPrefix();
    if (cmpVersion(tesseract.Version(), "4.0.0") >= 0)
      TessdataPrefix += "tessdata/";

    getTimeMonotonic(&e);

    cout << "OpenALPR getTessdataPrefix Time: " << diffclock(s, e) << "ms." << endl;

    // Tesseract requires the prefix directory to be set as an env variable
    getTimeMonotonic(&s);
    tesseract.Init(TessdataPrefix.c_str(), config->ocrLanguage.c_str() 	);
    getTimeMonotonic(&e);

    cout << "OpenALPR Init Time: " << diffclock(s, e) << "ms." << endl;

    getTimeMonotonic(&s);
    tesseract.SetVariable("save_blob_choices", "T");
    tesseract.SetVariable("debug_file", "/dev/null");
    tesseract.SetPageSegMode(PSM_SINGLE_CHAR);
    getTimeMonotonic(&e);

    cout << "OpenALPR SetVariable Time: " << diffclock(s, e) << "ms." << endl;
  }

  TesseractOcr::~TesseractOcr()
  {
    tesseract.End();
  }

  std::vector<OcrChar> TesseractOcr::recognize_line(int line_idx, PipelineData* pipeline_data) {
    timespec startTime;
    getTimeMonotonic(&startTime);

    const int SPACE_CHAR_CODE = 32;

    std::vector<OcrChar> recognized_chars;

    std::vector<tesseract::ResultIterator> riVector;
    std::vector<int> absolute_charpos_vector;
    for (unsigned int i = 0; i < pipeline_data->thresholds.size(); i++)
    {
      // Make it black text on white background
      bitwise_not(pipeline_data->thresholds[i], pipeline_data->thresholds[i]);
      tesseract.SetImage((uchar*) pipeline_data->thresholds[i].data,
                          pipeline_data->thresholds[i].size().width, pipeline_data->thresholds[i].size().height,
                          pipeline_data->thresholds[i].channels(), pipeline_data->thresholds[i].step1());


      int absolute_charpos = 0;

      std::cout << " OCR charRegions size: " <<  pipeline_data->charRegions[line_idx].size() << std::endl;

      for (unsigned int j = 0; j < pipeline_data->charRegions[line_idx].size(); j++)
      {
        Rect expandedRegion = expandRect( pipeline_data->charRegions[line_idx][j], 2, 2, pipeline_data->thresholds[i].cols, pipeline_data->thresholds[i].rows) ;

        tesseract.SetRectangle(expandedRegion.x, expandedRegion.y, expandedRegion.width, expandedRegion.height);
        tesseract.Recognize(NULL); // TODO: recognize

        tesseract::ResultIterator* ri = tesseract.GetIterator();
        tesseract::PageIteratorLevel level = tesseract::RIL_SYMBOL;

        std::vector<tesseract::ResultIterator> riVector;

        tesseract::ResultIterator* r2 = tesseract.GetIterator();
        do {
          riVector.push_back(*ri);
          absolute_charpos_vector.push_back(absolute_charpos);
        } while((ri->Next(level)));

        // delete ri;

        absolute_charpos++;
      }
    }

    omp_set_num_threads(1);
    #pragma omp parallel
    {
        std::vector<OcrChar> private_recognized_chars;
        tesseract::PageIteratorLevel level = tesseract::RIL_SYMBOL;

        #pragma omp for nowait schedule(static)
        for (int p = 0; p < riVector.size(); p++) {
          tesseract::ResultIterator r = riVector[p];
          int absolute_charpos = absolute_charpos_vector[p];

          const char* symbol = r.GetUTF8Text(level);
          float conf = r.Confidence(level);

          bool dontcare;
          int fontindex = 0;
          int pointsize = 0;
          const char* fontName = r.WordFontAttributes(&dontcare, &dontcare, &dontcare, &dontcare, &dontcare, &dontcare, &pointsize, &fontindex);

          // Ignore NULL pointers, spaces, and characters that are way too small to be valid
          if(symbol != 0 && symbol[0] != SPACE_CHAR_CODE && pointsize >= config->ocrMinFontSize)
          {
            OcrChar c;
            c.char_index = absolute_charpos;
            c.confidence = conf;
            c.letter = string(symbol);
            private_recognized_chars.push_back(c);

            // if (this->config->debugOcr)
            //   printf("charpos%d line%d: threshold %d:  symbol %s, conf: %f font: %s (index %d) size %dpx", absolute_charpos, line_idx, i, symbol, conf, fontName, fontindex, pointsize);

            bool indent = false;
            tesseract::ChoiceIterator ci(r);
            do
            {
              const char* choice = ci.GetUTF8Text();

              OcrChar c2;
              c2.char_index = absolute_charpos;
              c2.confidence = ci.Confidence();
              c2.letter = string(choice);

              // std::cout << " -- HAISOHAN c2 char_index: " << c2.char_index << " confidence : " <<  c2.confidence << " letter : " << c2.letter << std::endl;

              //1/17/2016 adt adding check to avoid double adding same character if ci is same as symbol. Otherwise first choice from ResultsIterator will get added twice when choiceIterator run.
              if (string(symbol) != string(choice))
                private_recognized_chars.push_back(c2);
              else
              {
                // Explictly double-adding the first character.  This leads to higher accuracy right now, likely because other sections of code
                // have expected it and compensated.
                // TODO: Figure out how to remove this double-counting of the first letter without impacting accuracy
                private_recognized_chars.push_back(c2);
              }
              if (this->config->debugOcr)
              {
                if (indent) printf("\t\t ");
                printf("\t- ");
                printf("%s conf: %f\n", choice, ci.Confidence());
              }

              indent = true;
            } while(ci.Next());
          }
        }

        #pragma omp for schedule(static) ordered
        for(int p=0; p<omp_get_num_threads(); p++) {
            #pragma omp ordered
            recognized_chars.insert(recognized_chars.end(), private_recognized_chars.begin(), private_recognized_chars.end());
        }
    }

    for (int i=0; i<recognized_chars.size(); i++) {
      OcrChar c = recognized_chars[i];
      std::cout << " -- HAISOHAN " << i << " char_index: " << c.char_index << " confidence : " <<  c.confidence << " letter : " << c.letter << std::endl;
    }


    if (config->debugTiming)
    {
      timespec endTime;
      getTimeMonotonic(&endTime);
      std::cout << " -- HAISOHAN recognize_line Time: " << diffclock(startTime, endTime) << "ms." << std::endl;
    }

    return recognized_chars;
  }
  void TesseractOcr::segment(PipelineData* pipeline_data) {
    std::cout << "======== OCR TesseractOcr::segment " << std::endl;
    CharacterSegmenter segmenter(pipeline_data);
    segmenter.segment();
  }


}
