#include "gpu2d/javascript_page.hpp"
#include "gpu2d/builtin_examples.hpp"

#include <cstdlib>
#include <chrono>
#include <exception>
#include <iostream>
#include <stdexcept>

namespace {

void require(const bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

}  // namespace

int main() {
  try {
    std::uint64_t example_revision = 1;
    for (const gpu2d::BuiltinExample& example : gpu2d::builtinExamples()) {
      const auto example_page = gpu2d::JavaScriptPage::create(
          std::string(example.source), example_revision++);
      gpu2d::CanvasFrame example_frame;
      for (int frame_index = 0; frame_index < 3; ++frame_index) {
        example_frame = example_page->runAnimationFrame(frame_index * 16.667);
      }
      example_page->pointerMove(120.0F, 90.0F);
      require(!example_page->faulted(), "built-in example faulted during RAF execution");
      if (example.name.find("Long shadow") != std::string_view::npos) {
        require(example_frame.has_background_color, "CodePen CSS background was not parsed");
        require((example_frame.required_capabilities & gpu2d::CanvasCapabilityGradients) != 0U,
                "CodePen radial gradient capability was not requested");
        require(example_frame.gradient_stops.size() >= 4U,
                "CodePen radial gradient stops were not recorded");
      }
    }

    const auto page = gpu2d::JavaScriptPage::create(
        R"HTML(<canvas id="c" width="320" height="180"></canvas>
<script>
const canvas = document.getElementById("c");
const ctx = canvas.getContext("2d");
ctx.fillStyle = "#ff0000";
ctx.fillRect(1, 2, 30, 40);
requestAnimationFrame(function frame(t) {
  ctx.fillStyle = "rgba(0, 0, 0, 0.1)";
  ctx.fillRect(0, 0, canvas.width, canvas.height);
});
</script>)HTML",
        7);
    require(page->canvasWidth() == 320, "canvas width mismatch");
    require(page->canvasHeight() == 180, "canvas height mismatch");
    const gpu2d::CanvasFrame frame = page->runAnimationFrame(16.0);
    require(frame.page_revision == 7, "page revision mismatch");
    require(frame.reset_surface, "first frame must reset surface");
    require(frame.operations.size() == 2, "top-level and RAF operations were not recorded");

    const auto path_page = gpu2d::JavaScriptPage::create(
        R"HTML(<canvas id="c" width="100" height="100"></canvas><script>
const ctx=document.getElementById('c').getContext('2d');
let cancelled=requestAnimationFrame(()=>ctx.fillRect(0,0,1,1));
cancelAnimationFrame(cancelled);
requestAnimationFrame(()=>{
  ctx.strokeStyle='#00ff00'; ctx.shadowColor='#00ff00'; ctx.shadowBlur=8;
  ctx.globalCompositeOperation='lighter';
  ctx.beginPath(); ctx.moveTo(10,10); ctx.lineTo(90,10); ctx.lineTo(50,90);
  ctx.closePath(); ctx.stroke();
});
</script>)HTML",
        8);
    const gpu2d::CanvasFrame path_frame = path_page->runAnimationFrame(32.0);
    require(path_frame.operations.size() == 1, "cancelAnimationFrame did not remove callback");
    require(path_frame.path_segments.size() == 4, "path segments were not recorded");
    require((path_frame.required_capabilities & gpu2d::CanvasCapabilityPaths) != 0,
            "path capability was not requested");
    require((path_frame.required_capabilities & gpu2d::CanvasCapabilityShadows) != 0,
            "shadow capability was not requested");
    require((path_frame.required_capabilities & gpu2d::CanvasCapabilityLighterBlend) != 0,
            "lighter capability was not requested");

    const auto ordered_page = gpu2d::JavaScriptPage::create(
        R"HTML(<canvas id="c" width="10" height="10"></canvas><script>
let second;
requestAnimationFrame(()=>{ cancelAnimationFrame(second); });
second=requestAnimationFrame(()=>{ document.getElementById('c').getContext('2d').fillRect(0,0,1,1); });
</script>)HTML",
        9);
    const gpu2d::CanvasFrame ordered_frame = ordered_page->runAnimationFrame(48.0);
    require(ordered_frame.operations.empty(),
            "an earlier RAF callback did not cancel a later callback in the same frame");

    const auto browser_demo_page = gpu2d::JavaScriptPage::create(
        R"HTML(<canvas id="c" width="100" height="80"></canvas><script>
var w=c.width=window.innerWidth,h=c.height=window.innerHeight,ctx=c.getContext('2d');
ctx.fillStyle='hsl(120,100%,50%)';ctx.shadowColor='hsla(240,100%,50%,50%)';
ctx.shadowBlur=4;ctx.fillRect(0,0,w,h);
window.addEventListener('resize',function(){c.width=window.innerWidth;c.height=window.innerHeight;});
requestAnimationFrame(function loop(){ctx.fillStyle='hsl(30.5,100%,55%)';ctx.fillRect(2,3,4,5);});
</script>)HTML",
        10);
    require(browser_demo_page->canvasWidth() == 100 && browser_demo_page->canvasHeight() == 80,
            "canvas named-property dimension assignment failed");
    browser_demo_page->resizeViewport(320, 180);
    require(browser_demo_page->canvasWidth() == 320 && browser_demo_page->canvasHeight() == 180,
            "resize listener did not update canvas dimensions");
    const gpu2d::CanvasFrame browser_demo_frame = browser_demo_page->runAnimationFrame(64.0);
    require(browser_demo_frame.reset_surface, "canvas dimension setter did not reset the surface");
    require(browser_demo_frame.operations.size() == 1,
            "resize reset did not discard old pixels or RAF draw was not recorded");

    const auto custom_id_page = gpu2d::JavaScriptPage::create(
        R"HTML(<canvas id="stage" width="40" height="30"></canvas><script>
if(document.getElementById('stage') !== stage) throw new Error('custom canvas id was not exposed');
stage.getContext('2d').fillRect(0,0,1,1);
</script>)HTML",
        11);
    require(custom_id_page->runAnimationFrame(70.0).operations.size() == 1,
            "custom canvas id lookup failed");

    const auto no_id_page = gpu2d::JavaScriptPage::create(
        R"HTML(<canvas width="40" height="30"></canvas><script>
if(document.getElementById('') !== null) throw new Error('id-less canvas matched an empty id');
if(typeof c !== 'undefined') throw new Error('id-less canvas created window.c');
document.querySelector('canvas').getContext('2d').fillRect(0,0,1,1);
</script>)HTML",
        12);
    require(no_id_page->runAnimationFrame(71.0).operations.size() == 1,
            "id-less canvas querySelector lookup failed");

    bool syntax_failed = false;
    try {
      static_cast<void>(gpu2d::JavaScriptPage::create(
          "<canvas id='c'></canvas><script>function ( {</script>", 13));
    } catch (const std::exception&) {
      syntax_failed = true;
    }
    require(syntax_failed, "invalid JavaScript did not fail initialization");

    const auto timeout_start = std::chrono::steady_clock::now();
    bool timeout_failed = false;
    try {
      static_cast<void>(gpu2d::JavaScriptPage::create(
          "<canvas id='c'></canvas><script>while(true){}</script>", 14));
    } catch (const std::exception& error) {
      timeout_failed = std::string(error.what()).find("timed out") != std::string::npos;
    }
    require(timeout_failed, "infinite JavaScript was not interrupted as a timeout");
    require(std::chrono::steady_clock::now() - timeout_start < std::chrono::seconds(2),
            "JavaScript timeout took too long");

    std::cout << "PASS JavaScript Canvas runtime\n";
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}