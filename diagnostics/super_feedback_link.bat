@echo off
setlocal
echo.
echo M2-A FeedbackLinkSample instrument entry (documentation only).
echo This bat does NOT start the game or collect samples.
echo.
echo Enable (session perf_tuning.ini):
echo   feedback_link_sample=1
echo Optional arm env (reserved; S1 Active is tuning-only):
echo   set IFSS_FEEDBACK_LINK_ARMED=1
echo.
echo Export path (created only when instrument is on and samples seal):
echo   state\sessions\^<session_id^>\diagnostics\feedback-link-samples.jsonl
echo.
echo Rules:
echo   - Default is OFF. Ordinary run.bat must not open this.
echo   - Instrument runs are NOT FPS acceptance.
echo   - Off-instrument smoke: late-window FPS drift vs quiet baseline ^<= 2%%.
echo.
echo After enabling tuning in a diagnostic session, use an authorized Critical/light
echo collector separately. Do not treat Full FPS as split evidence.
echo.
pause
exit /b 0