// wrapper does also include process.h

#include "wrapper.h"

#include "AAX_MIDIUtilities.h"

void AAX_CALLBACK AAXWrapper_AlgorithmProcessProc(
    SAAX_Wrapper_AlgorithmicContext *const inInstancesBegin[], const void *inInstancesEnd)
{
  // processing instances
  SAAX_Wrapper_AlgorithmicContext *AAX_RESTRICT instance = inInstancesBegin[0];
  for (SAAX_Wrapper_AlgorithmicContext *const *walk = inInstancesBegin; walk < inInstancesEnd; ++walk)
  {
    instance = *walk;
    SAAX_Wrapper_PrivateData *data = instance->mPrivateData;
    auto *plug = data->wrapper;

    plug->process(instance);  // passes the context to the plugin which passes it to the ProcessAdapter
  }
}

void ClapAsAAX::process(SAAX_Wrapper_AlgorithmicContext *context)
{
  // abort any flush request
  _flushRequested.store(false);

  // process
  _processAdapter->process(context);
}

inline clap_beattime doubleToBeatTime(double t)
{
  return std::round(t * CLAP_BEATTIME_FACTOR);
}

inline clap_sectime doubleToSecTime(double t)
{
  return std::round(t * CLAP_SECTIME_FACTOR);
}

inline double midiDataByteToNormalized(uint8_t value)
{
  return static_cast<double>(value) / 127.0;
}

inline uint8_t normalizedToMidiDataByte(double value)
{
  return static_cast<uint8_t>(std::clamp(static_cast<int>(std::round(value * 127.0)), 0, 127));
}

inline uint8_t clampMidiDataByte(int32_t value)
{
  return static_cast<uint8_t>(std::clamp(value, int32_t{0}, int32_t{127}));
}

inline uint8_t clampMidiChannel(int32_t channel)
{
  return static_cast<uint8_t>(std::clamp(channel, int32_t{0}, int32_t{15}));
}

AAXProcessAdapter::~AAXProcessAdapter()
{
  delete[] _silent_input;
  delete[] _silent_output;
  delete[] _input_ports;
  delete[] _output_ports;
}

void AAXProcessAdapter::applyBusSetting(const clap_plugin_t *plugin, const char *buslayout,
                                        const clap_plugin_configurable_audio_ports_t *ext)
{
  if (ext)
  {
    // (1) convert buslayout to config
    // (2) apply setup to plugin
  }
}
void AAXProcessAdapter::setupProcessing(const clap_plugin_t *plugin, double samplerate,
                                        const clap_plugin_params_t *ext_param,
                                        const clap_plugin_audio_ports *ext_audio,
                                        Clap::IAutomation *automation,
                                        std::vector<clap_id> &gesturedparameters,
                                        ParamChangeQueue &inqueue, uint32_t midiportid, bool preferMIDI,
                                        bool hasMidiOutputNode)
{
  _plugin = plugin;
  _ext_param = ext_param;
  _automation = automation;
  _inqueue = &inqueue;
  _gesturedparameters = &gesturedparameters;

  _midi_first_portid = midiportid;
  _midi_prefer_mididialect = preferMIDI;
  _has_midi_output_node = hasMidiOutputNode;

  // other needed references like buffers, MIDINodes etc. are passed
  // via the SAAX_Wrapper_AlgorithmicContext to the process function
  // prepare the proc member

  // setup common transport. Set everything to zero and initialize header
  memset(&_transport, 0, sizeof(_transport));
  _transport.header = {
      sizeof(clap_event_transport_t), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_TRANSPORT,
      0  // flags
  };

  // setting up event communication structures
  _in_events = {this, input_events_size, input_events_get};

  _out_events = {this, output_events_try_push};

  auto numinputs = ext_audio->count(_plugin, true);
  auto numoutputs = ext_audio->count(_plugin, false);

  LOGDETAIL(fmt::format("setting up audio for '{}' with {} inputs and {} outputs", _plugin->desc->name,
                        numinputs, numoutputs));

  // TODO: adapt to number of ports
  _input_ports = nullptr;
  if (numinputs > 0)
  {
    _input_ports = new clap_audio_buffer_t[numinputs];  // for each input bus
  }
  _output_ports = nullptr;
  if (numoutputs > 0)
  {
    _output_ports = new clap_audio_buffer_t[numoutputs];  // for each output bus
  }

  for (uint32_t i = 0; i < numinputs; ++i)
  {
    clap_audio_port_info_t info;
    if (ext_audio->get(_plugin, i, true, &info))
    {
      _input_ports[i] = {nullptr, nullptr, info.channel_count, 0, 0};
      LOGDETAIL(fmt::format("    IN port {} with {} channels", i, info.channel_count));
    }
    else
    {
      LOGDETAIL(fmt::format("input port info for port {} can not be requested", i));
      _input_ports[i] = {nullptr, nullptr, 0, 0, 0};
    }
  }
  for (uint32_t i = 0; i < numoutputs; ++i)
  {
    clap_audio_port_info_t info;
    if (ext_audio->get(_plugin, i, false, &info))
    {
      _output_ports[i] = {nullptr, nullptr, info.channel_count, 0, 0};
      LOGDETAIL(fmt::format("    OUT port {} with {} channels", i, info.channel_count));
    }
    else
    {
      LOGDETAIL(fmt::format("input port info for port {} can not be requested", i));
      _output_ports[i] = {nullptr, nullptr, 0, 0, 0};
    }
  }

  // setup process structure
  _proc.steady_time = -1;  // change later
  _proc.frames_count =
      gAAXMaxBlockSizeInSamples;           // AAX usually uses 1024, may update during the process call
  _proc.transport = &_transport;           // point to transport field (updated during process call)
  _proc.audio_inputs_count = numinputs;    // TODO: update configuration accordingly
  _proc.audio_outputs_count = numoutputs;  // TODO: update configuration accordingly
  _proc.audio_inputs = _input_ports;
  _proc.audio_outputs = _output_ports;
  _proc.in_events = &_in_events;
  _proc.out_events = &_out_events;

  _samplerate = samplerate;

  _events.reserve(256);  // reserve enough  events for most things
  _eventindices.reserve(_events.capacity());
}

void AAXProcessAdapter::process(SAAX_Wrapper_AlgorithmicContext *context)
{
  // transport
  auto aax_transport = context->mTransportNode->GetTransport();

  // this clears the vectors (which do not resize to smaller)
  this->_events.clear();
  this->_eventindices.clear();

  // check transport
  if (aax_transport)
  {
    bool tmp;

    if (aax_transport->GetCurrentTempo(&_transport.tempo) == AAX_SUCCESS)
      _transport.flags |= CLAP_TRANSPORT_HAS_TEMPO;
    if (aax_transport->IsTransportPlaying(&tmp) == AAX_SUCCESS)
      _transport.flags |= CLAP_TRANSPORT_IS_PLAYING;

    {
      int64_t samplelocation;
      if (aax_transport->GetCurrentNativeSampleLocation(&samplelocation) == AAX_SUCCESS)
      {
        double loc = (double)samplelocation;
        _transport.song_pos_seconds = doubleToSecTime(loc / this->_samplerate);
        _transport.flags |= CLAP_TRANSPORT_HAS_SECONDS_TIMELINE;
        int32_t bars, beats;
        int64_t displayticks;
        if (aax_transport->GetBarBeatPosition(&bars, &beats, &displayticks, samplelocation) ==
            AAX_SUCCESS)
        {
          int32_t numerator = 4, denominator = 4;
          aax_transport->GetCurrentMeter(&numerator, &denominator);
          // bars and beats from AAX are 1-based; convert to beat count from song start
          double bar_start_beats = (double)(bars - 1) * numerator;
          double song_pos_beats = bar_start_beats + (double)(beats - 1);
          _transport.song_pos_beats = doubleToBeatTime(song_pos_beats);
          _transport.bar_start = doubleToBeatTime(bar_start_beats);
          _transport.bar_number = bars - 1;
          _transport.tsig_num = (uint16_t)numerator;
          _transport.tsig_denom = (uint16_t)denominator;
          _transport.flags |= CLAP_TRANSPORT_HAS_BEATS_TIMELINE | CLAP_TRANSPORT_HAS_TIME_SIGNATURE;
        }
        // loop position
        bool loops;
        int64_t loopstart, loopend;
        if (aax_transport->GetCurrentLoopPosition(&loops, &loopstart, &loopend) == AAX_SUCCESS)
        {
          if (loops)
          {
            _transport.flags |= CLAP_TRANSPORT_IS_LOOP_ACTIVE;
          }
          _transport.loop_start_seconds = doubleToSecTime((double)(loopstart) / _samplerate);
          _transport.loop_end_seconds = doubleToSecTime((double)(loopend) / _samplerate);
        }
      }
    }
    // TODO: More flags when appropriate
  }

  {
    clap_multi_event_t n;  // re-using the event, initializing everything we don't need twice
    n.header = {sizeof(clap_multi_event_t), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0};
    // AAX can not distinct between all of this, so set all to wildcard
    n.param.note_id = -1;
    n.param.port_index = -1;
    n.param.channel = -1;
    n.param.key = -1;

    // check inserted automation events, we do this before the MIDI notes, so param changes are already applied
    // to NOTE_ONs
    ParamChange c;
    while (_inqueue->pop(c))
    {
      n.param.param_id = c.paramID;
      n.param.value = c.value;
      n.param.cookie = c.cookie;

      _eventindices.push_back(_events.size());
      _events.emplace_back(n);
    }
  }

  // check MIDI IN
  if (context->mInputNode)
  {
    auto midiInputStream = context->mInputNode->GetNodeBuffer();
    const AAX_CMidiPacket *midiInPacketPtr = midiInputStream->mBuffer;
    auto numevents = midiInputStream->mBufferSize;

    while ((0 < numevents) && (NULL != midiInPacketPtr))
    {
      const auto eventFlags =
          static_cast<uint32_t>((midiInPacketPtr->mIsImmediate) ? CLAP_EVENT_IS_LIVE : 0);
      const auto eventTime = midiInPacketPtr->mTimestamp;
      const auto midiLength = midiInPacketPtr->mLength;
      const auto midiStatus = midiInPacketPtr->mData[0];

      if (midiLength == 0)
      {
        ++midiInPacketPtr;
        --numevents;
        continue;
      }

      if (midiLength >= 3 && AAX::IsNoteOff(midiInPacketPtr) && !_midi_prefer_mididialect)
      {
        clap_multi_event_t n{};
        n.note.header = {sizeof(clap_event_note_t), eventTime, CLAP_CORE_EVENT_SPACE_ID,
                         CLAP_EVENT_NOTE_OFF, eventFlags};
        n.note.channel = midiInPacketPtr->mData[0] & 0x0F;  // channel
        n.note.note_id = -1;
        n.note.port_index = _midi_first_portid;
        n.note.velocity = midiDataByteToNormalized(midiInPacketPtr->mData[2]);
        n.note.key = midiInPacketPtr->mData[1];
        _eventindices.push_back(_events.size());
        _events.emplace_back(n);
      }
      else if (midiLength >= 3 && AAX::IsNoteOn(midiInPacketPtr) && !_midi_prefer_mididialect)
      {
        clap_multi_event_t n{};
        n.note.header = {sizeof(clap_event_note_t), eventTime, CLAP_CORE_EVENT_SPACE_ID,
                         CLAP_EVENT_NOTE_ON, eventFlags};
        n.note.channel = midiInPacketPtr->mData[0] & 0x0F;  // channel
        n.note.note_id = -1;
        n.note.port_index = _midi_first_portid;
        n.note.velocity = midiDataByteToNormalized(midiInPacketPtr->mData[2]);
        n.note.key = midiInPacketPtr->mData[1];
        _eventindices.push_back(_events.size());
        _events.emplace_back(n);
      }
      else if ((midiStatus & 0XF0) < 0xF0)
      {
        clap_multi_event_t n{};
        n.midi.header = {sizeof(clap_event_midi_t), eventTime, CLAP_CORE_EVENT_SPACE_ID,
                         CLAP_EVENT_MIDI, eventFlags};
        n.midi.port_index = _midi_first_portid;
        n.midi.data[0] = midiInPacketPtr->mData[0];
        n.midi.data[1] = midiLength > 1 ? midiInPacketPtr->mData[1] : 0;
        n.midi.data[2] = midiLength > 2 ? midiInPacketPtr->mData[2] : 0;

        _eventindices.push_back(_events.size());
        _events.emplace_back(n);
      }
      else
      {
        // no sysex for now
      }

      ++midiInPacketPtr;
      --numevents;
    }
  }

  _proc.frames_count = *(context->mNumSamples);

  // distribute the pointers to the audio channels pointer arrays to the
  // appropriate audio ports
  uint32_t offset = 0;
  for (uint32_t i = 0; i < _proc.audio_inputs_count; ++i)
  {
    this->_input_ports[i].data32 = context->mAudioInputs + offset;
    offset += this->_input_ports[i].channel_count;
  }
  offset = 0;
  for (uint32_t i = 0; i < _proc.audio_outputs_count; ++i)
  {
    this->_output_ports[i].data32 = context->mAudioOutputs + offset;
    offset += this->_output_ports[i].channel_count;
  }

  // sort all indices
  sortEventIndices();

  _current_context = context;
  auto status = _plugin->process(_plugin, &_proc);
  _current_context = nullptr;
  switch (status)
  {
    case CLAP_PROCESS_ERROR:
      // erase the output buffers on error - signal error how?
      for (uint32_t i = 0; i < _proc.audio_outputs_count; ++i)
      {
        for (uint32_t c = 0; c < _output_ports[i].channel_count; ++c)
        {
          memset(_output_ports[i].data32[c], 0, sizeof(float) * (_proc.frames_count));
        }
      }
      break;

      // Processing succeeded, keep processing.
    case CLAP_PROCESS_CONTINUE:
      // Processing succeeded, keep processing if the output is not quiet.
    case CLAP_PROCESS_CONTINUE_IF_NOT_QUIET:
      // Rely upon the plugin's tail to determine if the plugin should continue to process.
      // see clap_plugin_tail
    case CLAP_PROCESS_TAIL:
      // Processing succeeded, but no more processing is required,
      // until the next event or variation in audio input.
    case CLAP_PROCESS_SLEEP:
      break;
  }

}

void AAXProcessAdapter::flush()
{
  this->_ext_param->flush(_plugin, &_in_events, &_out_events);
}

void AAXProcessAdapter::sortEventIndices()
{
  // just sorting the index
  // an item must be sorted to front of
  // if the timestamp if event[a] is earlier than
  // the timestamp of event[b].
  // if they have the same timestamp, the index must be preserved

  std::sort(_eventindices.begin(), _eventindices.end(),
            [&](size_t const &a, size_t const &b)
            {
              auto t1 = _events[a].header.time;
              auto t2 = _events[b].header.time;
              return (t1 == t2) ? (a < b) : (t1 < t2);
            });
}

bool AAXProcessAdapter::output_events_try_push(const clap_output_events *list,
                                               const clap_event_header_t *event)
{
  auto self = static_cast<AAXProcessAdapter *>(list->ctx);
  // mainly used for CLAP_EVENT_NOTE_CHOKE and CLAP_EVENT_NOTE_END
  // but also for parameter changes
  return self->enqueueOutputEvent(event);
}

bool AAXProcessAdapter::enqueueOutputEvent(const clap_event_header_t *event)
{
  switch (event->type)
  {
    case CLAP_EVENT_NOTE_ON:
    {
      auto nevt = reinterpret_cast<const clap_event_note *>(event);
      return postMidiPacket(nevt->header.time, static_cast<uint8_t>(0x90u | clampMidiChannel(nevt->channel)),
                            clampMidiDataByte(nevt->key),
                            normalizedToMidiDataByte(nevt->velocity), 3);
    }
    case CLAP_EVENT_NOTE_OFF:
    {
      auto nevt = reinterpret_cast<const clap_event_note *>(event);
      return postMidiPacket(nevt->header.time, static_cast<uint8_t>(0x80u | clampMidiChannel(nevt->channel)),
                            clampMidiDataByte(nevt->key),
                            normalizedToMidiDataByte(nevt->velocity), 3);
    }
    case CLAP_EVENT_NOTE_END:
    case CLAP_EVENT_NOTE_CHOKE:
      removeFromActiveNotes((const clap_event_note *)(event));
      return true;
      break;
    case CLAP_EVENT_NOTE_EXPRESSION:
      return true;
      break;
    case CLAP_EVENT_PARAM_VALUE:
    {
      auto ev = (clap_event_param_value *)event;
      _automation->onPerformEdit(ev);
    }

      return true;
      break;
    case CLAP_EVENT_PARAM_MOD:
      return true;
      break;
    case CLAP_EVENT_PARAM_GESTURE_BEGIN:
    {
      auto ev = (clap_event_param_gesture *)event;
      _automation->onBeginEdit(ev->param_id);
    }
      return true;

      break;
    case CLAP_EVENT_PARAM_GESTURE_END:
    {
      auto ev = (clap_event_param_gesture *)event;
      _automation->onEndEdit(ev->param_id);
    }
      return true;
      break;

    case CLAP_EVENT_MIDI:
    {
      auto mevt = reinterpret_cast<const clap_event_midi *>(event);
      const auto status = static_cast<uint8_t>(mevt->data[0]);
      const auto kind = static_cast<uint8_t>(status & 0xF0u);
      switch (kind)
      {
        case 0x80:
        case 0x90:
        case 0xA0:
        case 0xB0:
        case 0xE0:
          return postMidiPacket(mevt->header.time, status, mevt->data[1], mevt->data[2], 3);
        case 0xC0:
        case 0xD0:
          return postMidiPacket(mevt->header.time, status, mevt->data[1], 0, 2);
        default:
          return false;
      }
    }
    case CLAP_EVENT_MIDI_SYSEX:
    case CLAP_EVENT_MIDI2:
      return false;
      break;
    default:
      break;
  }
  return false;
}

bool AAXProcessAdapter::postMidiPacket(uint32_t timestamp, uint8_t status, uint8_t data1,
                                       uint8_t data2, uint32_t length)
{
  if (!_has_midi_output_node || !_current_context || !_current_context->mOutputNode || length < 1 ||
      length > 3)
  {
    return false;
  }

  AAX_CMidiPacket packet{};
  packet.mIsImmediate = false;
  packet.mTimestamp = timestamp;
  if (_proc.frames_count > 0)
  {
    packet.mTimestamp = std::min(packet.mTimestamp, static_cast<uint32_t>(_proc.frames_count - 1));
  }
  packet.mLength = length;
  packet.mData[0] = status;
  packet.mData[1] = data1;
  packet.mData[2] = data2;

  return _current_context->mOutputNode->PostMIDIPacket(&packet) == AAX_SUCCESS;
}

void AAXProcessAdapter::addToActiveNotes(const clap_event_note *note)
{
  for (auto &i : _activeNotes)
  {
    if (!i.used)
    {
      i.note_id = note->note_id;
      i.port_index = note->port_index;
      i.channel = note->channel;
      i.key = note->key;
      i.used = true;
      return;
    }
  }
  _activeNotes.emplace_back(ActiveNote{true, note->note_id, note->port_index, note->channel, note->key});
}

void AAXProcessAdapter::removeFromActiveNotes(const clap_event_note *note)
{
  for (auto &i : _activeNotes)
  {
    if (i.used && i.port_index == note->port_index && i.channel == note->channel &&
        i.note_id == note->note_id)
    {
      i.used = false;
    }
  }
}

uint32_t AAXProcessAdapter::input_events_size(const struct clap_input_events *list)
{
  auto self = static_cast<AAXProcessAdapter *>(list->ctx);
  return (uint32_t)self->_events.size();
}

const clap_event_header_t *AAXProcessAdapter::input_events_get(const struct clap_input_events *list,
                                                               uint32_t index)
{
  auto self = static_cast<AAXProcessAdapter *>(list->ctx);
  if (self->_events.size() > index)
  {
    // we can safely return the note.header also for other event types
    // since they are at the same memory address
    auto realindex = self->_eventindices[index];
    return &(self->_events[realindex].header);
  }
  return nullptr;
}
