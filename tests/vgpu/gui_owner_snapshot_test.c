/** @file gui_owner_snapshot_test.c @brief Actual failure observer over existing fixture API doubles.
 * Reuse the existing synthetic setup without changing its tests or owner gates.
 * Every diagnostic must leave all owner bytes and accepted boundary state intact.
 */
#define _GNU_SOURCE
#define main existing_fixture_controls
#include "m2_owned_fixture_test.c"
#undef main
int main(void) {
    Check(existing_fixture_controls(1,NULL)==0);
    reset();Check(m2_owned_begin(Candidate,9));
    HWND window=create_window();Check(window!=NULL);
    HDC dc=m2_owned_get_dc(window);Check(dc!=NULL);
    m2_owned.lifetime=2;m2_owned.initialized=1;
    m2_owned_fixture_t before=m2_owned;
    for(unsigned failed=0;failed<2;++failed) {
        query_ok[1]=!failed;
        unsigned old_icd_calls=query_calls[0],old_dc_calls=query_calls[1];
        char *text=NULL;size_t bytes=0;FILE *stream=open_memstream(&text,&bytes);Check(stream!=NULL);
        m2_owned_gui_failure(stream);Check(fclose(stream)==0);
        Check(!memcmp(&before,&m2_owned,sizeof before));
        Check(query_calls[0]==old_icd_calls && query_calls[1]==old_dc_calls+1);
        Check(strstr(text,"last_accepted_lifetime=2 failure_observation_only=1"));
        Check(strstr(text,"owner_group=fixture_window query=1") && strstr(text,"live=1"));
        Check(!strstr(text,"M2_OWNED_V1") && !strstr(text,"PASS"));
        Check(strstr(text,failed ? "owner_group=sink_dc query=0 unavailable=1" : "owner_group=sink_dc query=1 version=1"));
        free(text);
    }
    Check(m2_owned_release_dc(window,dc));Check(m2_owned_destroy_window(window));
    reset();char *text=NULL;size_t bytes=0;FILE *stream=open_memstream(&text,&bytes);Check(stream!=NULL);
    m2_owned_gui_failure(NULL);m2_owned_gui_failure(stream);Check(fclose(stream)==0);
    Check(strstr(text,"ownership_mode=0 ledgers_unavailable=1") && !query_calls[0] && !query_calls[1]);free(text);
    puts("GUI_OWNER_SNAPSHOT_PASS (API doubles, immutable failure observation)");return 0;
}
