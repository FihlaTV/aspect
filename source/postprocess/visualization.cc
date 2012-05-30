/*
  Copyright (C) 2011, 2012 by the authors of the ASPECT code.

  This file is part of ASPECT.

  ASPECT is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2, or (at your option)
  any later version.

  ASPECT is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with ASPECT; see the file doc/COPYING.  If not see
  <http://www.gnu.org/licenses/>.
*/
/*  $Id$  */


#include <aspect/postprocess/visualization.h>
#include <aspect/simulator.h>
#include <aspect/global.h>

#include <deal.II/dofs/dof_tools.h>
#include <deal.II/numerics/data_out.h>

#include <boost/archive/text_oarchive.hpp>
#include <boost/archive/text_iarchive.hpp>

#include <math.h>
#include <stdio.h>

namespace aspect
{
  namespace Postprocess
  {
    namespace VisualizationPostprocessors
    {
      template <int dim>
      Interface<dim>::~Interface ()
      {}


      template <int dim>
      void
      Interface<dim>::declare_parameters (ParameterHandler &)
      {}



      template <int dim>
      void
      Interface<dim>::parse_parameters (ParameterHandler &)
      {}



      template <int dim>
      void
      Interface<dim>::save (std::map<std::string,std::string> &) const
      {}


      template <int dim>
      void
      Interface<dim>::load (const std::map<std::string,std::string> &)
      {}
    }


    template <int dim>
    Visualization<dim>::Visualization ()
      :
      // the following value is later read from the input file
      output_interval (0),
      // initialize this to a nonsensical value; set it to the actual time
      // the first time around we get to check it
      next_output_time (std::numeric_limits<double>::quiet_NaN()),
      output_file_number (0)
    {}



    template <int dim>
    Visualization<dim>::~Visualization ()
    {
      // make sure a thread that may still be running in the background,
      // writing data, finishes
      background_thread.join ();
    }



    template <int dim>
    std::pair<std::string,std::string>
    Visualization<dim>::execute (TableHandler &statistics)
    {
      // if this is the first time we get here, set the next output time
      // to the current time. this makes sure we always produce data during
      // the first time step
      if (std::isnan(next_output_time))
        next_output_time = this->get_time();

      // see if graphical output is requested at this time
      if (this->get_time() < next_output_time)
        return std::pair<std::string,std::string>();

      Vector<float> estimated_error_per_cell(this->get_triangulation().n_active_cells());
      this->get_refinement_criteria(estimated_error_per_cell);

//TODO: try to somehow get these variables into the viz plugins as well...
      Vector<float> Vs_anomaly(this->get_triangulation().n_active_cells());
      this->get_Vs_anomaly(Vs_anomaly);
      Vector<float> Vp_anomaly(this->get_triangulation().n_active_cells());
      this->get_Vp_anomaly(Vp_anomaly);

      // create a DataOut object on the heap; ownership of this
      // object will later be transferred to a different thread
      // that will write data in the background. the other thread
      // will then also destroy the object
      DataOut<dim> data_out;
      data_out.attach_dof_handler (this->get_dof_handler());

      // add the primary variables
      std::vector<std::string> solution_names (dim, "velocity");
      solution_names.push_back ("p");
      solution_names.push_back ("T");

      std::vector<DataComponentInterpretation::DataComponentInterpretation>
      interpretation (dim,
                      DataComponentInterpretation::component_is_part_of_vector);
      interpretation.push_back (DataComponentInterpretation::component_is_scalar);
      interpretation.push_back (DataComponentInterpretation::component_is_scalar);

      data_out.add_data_vector (this->get_solution(),
                                solution_names,
                                DataOut<dim>::type_dof_data,
                                interpretation);

      // then for each additional selected output variable
      // add the computed quantity as well
      for (typename std::list<std_cxx1x::shared_ptr<VisualizationPostprocessors::Interface<dim> > >::const_iterator
           p = postprocessors.begin(); p!=postprocessors.end(); ++p)
        {
          try
            {
              DataPostprocessor<dim> *viz_postprocessor
                = dynamic_cast<DataPostprocessor<dim>*>(& **p);
              Assert (viz_postprocessor != 0,
                      ExcInternalError());
              data_out.add_data_vector (this->get_solution(), *viz_postprocessor);
            }
          // viz postprocessors that throw exceptions usually do not result in
          // anything good because they result in an unwinding of the stack
          // and, if only one processor triggers an exception, the
          // destruction of objects often causes a deadlock. thus, if
          // an exception is generated, catch it, print an error message,
          // and abort the program
          catch (std::exception &exc)
            {
              std::cerr << std::endl << std::endl
                        << "----------------------------------------------------"
                        << std::endl;
              std::cerr << "Exception on MPI process <"
                        << Utilities::MPI::this_mpi_process(MPI_COMM_WORLD)
                        << "> while running visualization postprocessor <"
                        << typeid(**p).name()
                        << ">: " << std::endl
                        << exc.what() << std::endl
                        << "Aborting!" << std::endl
                        << "----------------------------------------------------"
                        << std::endl;

              // terminate the program!
              MPI_Abort (MPI_COMM_WORLD, 1);
            }
          catch (...)
            {
              std::cerr << std::endl << std::endl
                        << "----------------------------------------------------"
                        << std::endl;
              std::cerr << "Exception on MPI process <"
                        << Utilities::MPI::this_mpi_process(MPI_COMM_WORLD)
                        << "> while running visualization postprocessor <"
                        << typeid(**p).name()
                        << ">: " << std::endl;
              std::cerr << "Unknown exception!" << std::endl
                        << "Aborting!" << std::endl
                        << "----------------------------------------------------"
                        << std::endl;

              // terminate the program!
              MPI_Abort (MPI_COMM_WORLD, 1);
            }

        }

      data_out.add_data_vector (estimated_error_per_cell, "error_indicator");
      data_out.add_data_vector (Vs_anomaly, "Vs_anomaly");
      data_out.add_data_vector (Vp_anomaly, "Vp_anomaly");
      data_out.build_patches ();

//TODO: There is some code duplication between the following two
//code blocks. unify!
      if (output_format=="vtu" && group_files!=0)
        {
          AssertThrow(group_files==1, ExcNotImplemented());
          data_out.write_vtu_in_parallel((this->get_output_directory() + std::string("solution-") +
                                          Utilities::int_to_string (output_file_number, 5) +
                                          ".vtu").c_str(), MPI_COMM_WORLD);

          if (Utilities::MPI::this_mpi_process(this->get_mpi_communicator()) == 0)
            {
              std::vector<std::string> filenames;
              filenames.push_back (std::string("solution-") +
                                   Utilities::int_to_string (output_file_number, 5) +
                                   ".vtu");
              const std::string
              pvtu_master_filename = (this->get_output_directory() +
                                      "solution-" +
                                      Utilities::int_to_string (output_file_number, 5) +
                                      ".pvtu");
              std::ofstream pvtu_master (pvtu_master_filename.c_str());
              data_out.write_pvtu_record (pvtu_master, filenames);

              // now also generate a .pvd file that matches simulation
              // time and corresponding .pvtu record
              times_and_pvtu_names.push_back(std::pair<double,std::string>
                                             (this->get_time(), pvtu_master_filename));
              const std::string
              pvd_master_filename = (this->get_output_directory() + "solution.pvd");
              std::ofstream pvd_master (pvd_master_filename.c_str());
              data_out.write_pvd_record (pvd_master, times_and_pvtu_names);

              // finally, do the same for Visit via the .visit file
              const std::string
              visit_master_filename = (this->get_output_directory() +
                                       "solution-" +
                                       Utilities::int_to_string (output_file_number, 5) +
                                       ".visit");
              std::ofstream visit_master (visit_master_filename.c_str());
              data_out.write_visit_record (visit_master, filenames);
            }
        }
      else
        {
          // put the stuff we want to write into a string object that
          // we can then write in the background
          const std::string *file_contents;
          {
            std::ostringstream tmp;
            data_out.write (tmp, DataOutBase::parse_output_format(output_format));

            file_contents = new std::string (tmp.str());
          }

          // let the master processor write the master record for all the distributed
          // files
          if (Utilities::MPI::this_mpi_process(this->get_mpi_communicator()) == 0)
            {
              std::vector<std::string> filenames;
              for (unsigned int i=0; i<Utilities::MPI::n_mpi_processes(this->get_mpi_communicator()); ++i)
                filenames.push_back (std::string("solution-") +
                                     Utilities::int_to_string (output_file_number, 5) +
                                     "." +
                                     Utilities::int_to_string(i, 4) +
                                     DataOutBase::default_suffix
                                     (DataOutBase::parse_output_format(output_format)));
              const std::string
              pvtu_master_filename = (this->get_output_directory() +
                                      "solution-" +
                                      Utilities::int_to_string (output_file_number, 5) +
                                      ".pvtu");
              std::ofstream pvtu_master (pvtu_master_filename.c_str());
              data_out.write_pvtu_record (pvtu_master, filenames);

              // now also generate a .pvd file that matches simulation
              // time and corresponding .pvtu record
              times_and_pvtu_names.push_back(std::pair<double,std::string>
                                             (this->get_time(), pvtu_master_filename));
              const std::string
              pvd_master_filename = (this->get_output_directory() + "solution.pvd");
              std::ofstream pvd_master (pvd_master_filename.c_str());
              data_out.write_pvd_record (pvd_master, times_and_pvtu_names);

              // finally, do the same for Visit via the .visit file
              const std::string
              visit_master_filename = (this->get_output_directory() +
                                       "solution-" +
                                       Utilities::int_to_string (output_file_number, 5) +
                                       ".visit");
              std::ofstream visit_master (visit_master_filename.c_str());
              data_out.write_visit_record (visit_master, filenames);
            }

          const std::string *filename
            = new std::string (this->get_output_directory() +
                               "solution-" +
                               Utilities::int_to_string (output_file_number, 5) +
                               "." +
                               Utilities::int_to_string
                               (this->get_triangulation().locally_owned_subdomain(), 4) +
                               DataOutBase::default_suffix
                               (DataOutBase::parse_output_format(output_format)));

          // wait for all previous write operations to finish, should
          // any be still active
          background_thread.join ();

          // then continue with writing our own stuff
          background_thread = Threads::new_thread (&background_writer,
                                                   filename,
                                                   file_contents);
        }

      // record the file base file name in the output file
      statistics.add_value ("Visualization file name",
                            this->get_output_directory() + "solution-" +
                            Utilities::int_to_string (output_file_number, 5));

      // up the counter of the number of the file by one; also
      // up the next time we need output
      ++output_file_number;
      set_next_output_time (this->get_time());

      // return what should be printed to the screen. note that we had
      // just incremented the number, so use the previous value
      return std::make_pair (std::string ("Writing graphical output:"),
                             this->get_output_directory() +"solution-" +
                             Utilities::int_to_string (output_file_number-1, 5));
    }


    template <int dim>
    void Visualization<dim>::background_writer (const std::string *filename,
                                                const std::string *file_contents)
    {
      // write stuff into a (hopefully local) tmp file first. to do so first
      // find out whether $TMPDIR is set and if so put the file in there
      char tmp_filename[1025], *tmp_filedir;
      int tmp_file_desc;
      FILE *out_fp;
      static bool wrote_warning = false;
      bool using_bg_tmp = true;

      {
        // Try getting the environment variable for the temporary directory
        tmp_filedir = getenv("TMPDIR");
        // If we can't, default to /tmp
        sprintf(tmp_filename, "%s/tmp.XXXXXX", (tmp_filedir?tmp_filedir:"/tmp"));
        // Create the temporary file
        tmp_file_desc = mkstemp(tmp_filename);

        // If we failed to create the temp file, just write directly to the target file
        if (tmp_file_desc == -1)
          {
            if (!wrote_warning)
              std::cerr << "***** WARNING: could not create temporary file, will "
                        "output directly to final location. This may negatively "
                        "affect performance." << std::endl;
            wrote_warning = true;

            // Set the filename to be the specified input name
            sprintf(tmp_filename, "%s", filename->c_str());
            out_fp = fopen(tmp_filename, "w");
            using_bg_tmp = false;
          }
        else
          {
            out_fp = fdopen(tmp_file_desc, "w");
          }
      }

      // write the data into the file
      {
        if (!out_fp)
          {
            std::cerr << "***** ERROR: could not create " << tmp_filename
                      << " *****"
                      << std::endl;
          }
        else
          {
            fprintf(out_fp, "%s", file_contents->c_str());
            fclose(out_fp);
          }
      }

      if (using_bg_tmp)
        {
          // now move the file to its final destination on the global file system
          std::string command = std::string("mv ") + tmp_filename + " " + *filename;
          int error = system(command.c_str());
          if (error != 0)
            {
              std::cerr << "***** ERROR: could not move " << tmp_filename
                        << " to " << *filename << " *****"
                        << std::endl;
            }
        }

      // destroy the pointers to the data we needed to write
      delete file_contents;
      delete filename;
    }


    namespace
    {
      std_cxx1x::tuple
      <void *,
      void *,
      aspect::internal::Plugins::PluginList<VisualizationPostprocessors::Interface<2> >,
      aspect::internal::Plugins::PluginList<VisualizationPostprocessors::Interface<3> > > registered_plugins;
    }


    template <int dim>
    void
    Visualization<dim>::declare_parameters (ParameterHandler &prm)
    {
      prm.enter_subsection("Postprocess");
      {
        prm.enter_subsection("Visualization");
        {
          prm.declare_entry ("Time between graphical output", "1e8",
                             Patterns::Double (0),
                             "The time interval between each generation of "
                             "graphical output files. A value of zero indicates "
                             "that output should be generated in each time step. "
                             "Units: years if the "
                             "'Use years in output instead of seconds' parameter is set; "
                             "seconds otherwise.");

          // now also see about the file format we're supposed to write in
          prm.declare_entry ("Output format", "vtu",
                             Patterns::Selection (DataOutInterface<dim>::get_output_format_names ()),
                             "The file format to be used for graphical output.");

          prm.declare_entry ("Number of grouped files", "0",
                             Patterns::Integer(0),
                             "VTU file output supports grouping files from several CPUs "
                             "into one file using MPI I/O when writing on a parallel "
                             "filesystem. Select 0 for no grouping. This will disable "
                             "parallel file output and instead write one file per processor "
                             "in a background thread. "
                             "A value of 1 will generate one big file containing the whole "
                             "solution.");

          // finally also construct a string for Patterns::MultipleSelection that
          // contains the names of all registered visualization postprocessors
          const std::string pattern_of_names
            = std_cxx1x::get<dim>(registered_plugins).get_pattern_of_names (true);
          prm.declare_entry("List of output variables",
                            "",
                            Patterns::MultipleSelection(pattern_of_names),
                            "A comma separated list of visualization objects that should be run "
                            "whenever writing graphical output. By default, the graphical "
                            "output files will always contain the primary variables velocity, "
                            "pressure, and temperature. However, one frequently wants to also "
                            "visualize derived quantities, such as the thermodynamic phase "
                            "that corresponds to a given temperature-pressure value, or the "
                            "corresponding seismic wave speeds. The visualization objects do "
                            "exactly this: they compute such derived quantities and place them "
                            "into the output file. The current parameter is the place where "
                            "you decide which of these additional output variables you want "
                            "to have in your output file.\n\n"
                            "The following postprocessors are available:\n\n"
                            +
                            std_cxx1x::get<dim>(registered_plugins).get_description_string());
        }
        prm.leave_subsection();
      }
      prm.leave_subsection();
    }


    template <int dim>
    void
    Visualization<dim>::parse_parameters (ParameterHandler &prm)
    {
      Assert (std_cxx1x::get<dim>(registered_plugins).plugins != 0,
              ExcMessage ("No postprocessors registered!?"));

      prm.enter_subsection("Postprocess");
      {
        prm.enter_subsection("Visualization");
        {
          output_interval = prm.get_double ("Time between graphical output");
          output_format   = prm.get ("Output format");
          group_files     = prm.get_integer("Number of grouped files");

          // now also see which derived quantities we are to compute
          std::vector<std::string> viz_names
            = Utilities::split_string_list(prm.get("List of output variables"));

          // see if 'all' was selected (or is part of the list). if so
          // simply replace the list with one that contains all names
          if (std::find (viz_names.begin(),
                         viz_names.end(),
                         "all") != viz_names.end())
            {
              viz_names.clear();
              for (typename std::list<typename aspect::internal::Plugins::PluginList<VisualizationPostprocessors::Interface<dim> >::PluginInfo>::const_iterator
                   p = std_cxx1x::get<dim>(registered_plugins).plugins->begin();
                   p != std_cxx1x::get<dim>(registered_plugins).plugins->end(); ++p)
                viz_names.push_back (std_cxx1x::get<0>(*p));
            }

          // then go through the list, create objects and let them parse
          // their own parameters
          for (unsigned int name=0; name<viz_names.size(); ++name)
            {
              VisualizationPostprocessors::Interface<dim> *
              viz_postprocessor = std_cxx1x::get<dim>(registered_plugins)
                                  .create_plugin (viz_names[name], prm);

              // make sure that the postprocessor is indeed of type
              // dealii::DataPostprocessor
              Assert (dynamic_cast<DataPostprocessor<dim>*>(viz_postprocessor)
                      != 0,
                      ExcMessage ("Can't convert visualization postprocessor to type "
                                  "dealii::DataPostprocessor!?"));

              postprocessors.push_back (std_cxx1x::shared_ptr<VisualizationPostprocessors::Interface<dim> >
                                        (viz_postprocessor));
            }
        }
        prm.leave_subsection();
      }
      prm.leave_subsection();
    }


    template <int dim>
    template <class Archive>
    void Visualization<dim>::serialize (Archive &ar, const unsigned int)
    {
      ar &next_output_time
      & output_file_number
      & times_and_pvtu_names;
    }


    template <int dim>
    void
    Visualization<dim>::save (std::map<std::string, std::string> &status_strings) const
    {
      std::ostringstream os;
      aspect::oarchive oa (os);
      oa << (*this);

      status_strings["Visualization"] = os.str();

//TODO: do something about the visualization postprocessor plugins
    }


    template <int dim>
    void
    Visualization<dim>::load (const std::map<std::string, std::string> &status_strings)
    {
      // see if something was saved
      if (status_strings.find("Visualization") != status_strings.end())
        {
          std::istringstream is (status_strings.find("Visualization")->second);
          aspect::iarchive ia (is);
          ia >> (*this);
        }

//TODO: do something about the visualization postprocessor plugins

      // set next output time to something useful
      set_next_output_time (this->get_time());
    }


    template <int dim>
    void
    Visualization<dim>::set_next_output_time (const double current_time)
    {
      // if output_interval is positive, then set the next output interval to
      // a positive multiple; we need to interpret output_interval either
      // as years or as seconds
      if (output_interval > 0)
        {
          if (this->convert_output_to_years() == true)
            next_output_time = std::ceil(current_time / (output_interval * year_in_seconds)) *
                               (output_interval * year_in_seconds);
          else
            next_output_time = std::ceil(current_time / (output_interval )) *
                               output_interval;
        }
    }


    template <int dim>
    void
    Visualization<dim>::initialize (const Simulator<dim> &simulator)
    {
      // first call the respective function in the base class
      SimulatorAccess<dim>::initialize (simulator);

      // pass initialization through to the various visualization
      // objects if they so desire
      for (typename std::list<std_cxx1x::shared_ptr<VisualizationPostprocessors::Interface<dim> > >::iterator
           p = postprocessors.begin();
           p != postprocessors.end(); ++p)
        // see if a given visualization plugin is in fact derived
        // from the SimulatorAccess class, and if so initialize it.
        // note that viz plugins need not necessarily derive from
        // SimulatorAccess if they don't need anything beyond the
        // solution variables to compute what they compute
        if (SimulatorAccess<dim> * x = dynamic_cast<SimulatorAccess<dim>*>(& **p))
          x->initialize (simulator);
    }


    template <int dim>
    void
    Visualization<dim>::
    register_visualization_postprocessor (const std::string &name,
                                          const std::string &description,
                                          void (*declare_parameters_function) (ParameterHandler &),
                                          VisualizationPostprocessors::Interface<dim> *(*factory_function) ())
    {
      std_cxx1x::get<dim>(registered_plugins).register_plugin (name,
                                                               description,
                                                               declare_parameters_function,
                                                               factory_function);
    }

  }
}


// explicit instantiations
namespace aspect
{
  namespace Postprocess
  {
    namespace VisualizationPostprocessors
    {
#define INSTANTIATE(dim) \
  template class Interface<dim>;

      ASPECT_INSTANTIATE(INSTANTIATE)
    }
  }

  namespace internal
  {
    namespace Plugins
    {
      template <>
      std::list<internal::Plugins::PluginList<Postprocess::VisualizationPostprocessors::Interface<2> >::PluginInfo> *
      internal::Plugins::PluginList<Postprocess::VisualizationPostprocessors::Interface<2> >::plugins = 0;
      template <>
      std::list<internal::Plugins::PluginList<Postprocess::VisualizationPostprocessors::Interface<3> >::PluginInfo> *
      internal::Plugins::PluginList<Postprocess::VisualizationPostprocessors::Interface<3> >::plugins = 0;
    }
  }


  namespace Postprocess
  {
    ASPECT_REGISTER_POSTPROCESSOR(Visualization,
                                  "visualization",
                                  "A postprocessor that takes the solution and writes "
                                  "it into files that can be read by a graphical "
                                  "visualization program. Additional run time parameters "
                                  "are read from the parameter subsection 'Visualization'.")
  }
}
