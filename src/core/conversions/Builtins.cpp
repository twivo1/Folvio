#include "core/conversions/ConversionRegistry.h"

#include "engines/data/TextDataConverter.h"
#include "engines/documents/LibreOfficeConverter.h"
#include "engines/documents/WordConverter.h"
#include "engines/images/ImageConverters.h"
#include "engines/media/FfmpegConverter.h"
#include "engines/pdf/PdfConverters.h"

namespace folvio {

void ConversionRegistry::registerBuiltins()
{
    auto& r = instance();
    r.add(std::make_unique<WordConverter>());
    r.add(std::make_unique<LibreOfficeConverter>());
    r.add(std::make_unique<PdfRasterConverter>());
    r.add(std::make_unique<ImagesToPdfConverter>());
    r.add(std::make_unique<QtImageConverter>());
    r.add(std::make_unique<FfmpegImageConverter>());
    r.add(std::make_unique<FfmpegConverter>());
    r.add(std::make_unique<TextDataConverter>());
}

} // namespace folvio
